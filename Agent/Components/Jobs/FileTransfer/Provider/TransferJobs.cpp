#include "TransferJobs.hpp"

#include <algorithm>
#include <chrono>

#include "Common/Protocol/CmdDef.hpp"
#include "Common/Logger/ErrLog.hpp"
#include "Common/Logger/Logger.hpp"
#include "Common/Protocol/FrameBuilder.hpp"

namespace cleat::Tasks {

    namespace {

        std::vector<uint8_t> ToBytes(const nlohmann::json& j) {
            const std::string s = j.dump();
            return std::vector<uint8_t>(s.begin(), s.end());
        }

        std::string TaskIdOf(uint64_t job_id) {
            return "task_" + std::to_string(job_id);
        }

    } // namespace

    // ---------------- 下载(循环驱动 job) ----------------

    void RunDownloadJob(JobContext& ctx, const std::string& path, int chunkSize, IFileOps& ops) {
        const std::string task_id = TaskIdOf(ctx.id());
        auto send_error = [&](const std::string& message) {
            nlohmann::json err;
            err["cmd"] = Cmd::FileManager::DownloadError();
            err["data"]["task_id"] = task_id;
            err["data"]["path"] = path;
            err["data"]["error"] = message;
            ctx.Send(ToBytes(err));
            };

        const uint64_t totalSize = ops.GetFileSize(path);
        // 行为保持(与旧实现一致): 0 一律按"cannot read file"处理(空文件下载属已登记的
        // 跨平台待修项, 本提交不扩协议行为)。
        if (totalSize == 0) {
            CLEAT_LOGE(ErrDom::Job, ErrCode::PlatformApiFailed,
                "DownloadJob: cannot read file " << path);
            send_error("Cannot read file");
            ctx.Fail();   // 业务失败 → 终态 Failed(而非 Completed; 真机冒烟修正)
            return;
        }

        const uint64_t totalChunks = totalSize / static_cast<uint64_t>(chunkSize)
            + (totalSize % static_cast<uint64_t>(chunkSize) ? 1 : 0);

        for (uint64_t i = 0; i < totalChunks; ++i) {
            // 分块粒度暂停/取消/收尾: 在飞块发完, 下一块在此等恢复或退出(与旧 gov.Tick 同位)
            if (!ctx.Tick()) return;

            const uint64_t offset = i * static_cast<uint64_t>(chunkSize);
            const size_t chunkLen = static_cast<size_t>(
                std::min<uint64_t>(static_cast<uint64_t>(chunkSize), totalSize - offset));

            std::vector<uint8_t> chunk;
            if (!ops.ReadFileChunk(path, offset, chunkLen, chunk) || chunk.empty()) {
                CLEAT_LOGE(ErrDom::Job, ErrCode::PlatformApiFailed,
                    "DownloadJob: read chunk failed at " << offset);
                send_error("Read chunk failed");
                ctx.Fail();
                return;
            }

            // 混合帧: JSON头 + '\0' + 原始二进制(推送载荷统一放 data) —— 与旧实现逐字一致
            nlohmann::json header;
            header["cmd"] = Cmd::FileManager::DownloadData();
            header["data"]["type"] = "download_chunk";
            header["data"]["task_id"] = task_id;
            header["data"]["chunk_index"] = i;
            header["data"]["total_chunks"] = totalChunks;
            header["data"]["size"] = chunk.size();

            auto frame = FrameBuilder::Build(header, chunk);

            // B2-6 背压 +  修正: 失败**重试同一块**(可打断 200ms 片), 不再"记账照走" ——
            // 丢块 = 下载损坏; 取消/收尾则放弃(不发 complete, 与旧 break 同语义)。
            bool sent = ctx.Send(frame);
            while (!sent) {
                if (ctx.Cancelled()) return;
                if (!ctx.SleepFor(std::chrono::milliseconds(200))) return;
                sent = ctx.Send(frame);
            }
        }

        // 完成帧: 取消/收尾不发(避免静默期出站; 与旧 running_ 判定同语义)
        if (!ctx.Tick()) return;
        nlohmann::json complete;
        complete["cmd"] = Cmd::FileManager::DownloadComplete();
        complete["data"]["task_id"] = task_id;
        complete["data"]["path"] = path;
        complete["data"]["total_size"] = totalSize;
        ctx.Send(ToBytes(complete));
        LOG_INFO("DownloadJob: complete, task=" << task_id << ", size=" << totalSize);
    }

    JobRuntime::TaskStart SpawnDownloadJob(const std::string& path, int chunkSize) {
        JobSpec spec;
        spec.name = "file-download";
        spec.kind = JobKind::Task;
        spec.high_bandwidth = true;   // 大文件流式推送: 低带宽通道拒绝启动/降级时取消

        return JobRuntime::StartTask(spec, [path, chunkSize](JobContext& ctx) {
            // L0-2(复核重开)修正: 平台 ops 为空(Linux 未实现)在入口判空报错, 不再解引用
            auto ops = CreateFileOps();
            if (!ops) {
                CLEAT_LOGE(ErrDom::Job, ErrCode::NotSupported,
                    "DownloadJob: 平台文件操作不可用(未实现?), path=" << path);
                nlohmann::json err;
                err["cmd"] = Cmd::FileManager::DownloadError();
                err["data"]["task_id"] = TaskIdOf(ctx.id());
                err["data"]["path"] = path;
                err["data"]["error"] = "Not supported on this platform";
                ctx.Send(ToBytes(err));
                return;
            }
            RunDownloadJob(ctx, path, chunkSize, *ops);
            });
    }

    // ---------------- 上传(会话驱动 job) ----------------

    UploadSession::UploadSession(std::string path, uint64_t totalSize, int totalChunks,
        std::shared_ptr<IFileOps> ops)
        : path_(std::move(path)), tmp_path_(path_ + ".uploading"),
          totalSize_(totalSize), totalChunks_(totalChunks), ops_(std::move(ops)) {
        // 防御: 拒绝超大上传(2GB 上限), 避免磁盘被恶意填充(与旧实现一致)
        if (totalSize_ > 2ULL * 1024 * 1024 * 1024) {
            CLEAT_LOGE(ErrDom::Job, ErrCode::ResourceExhausted,
                "UploadSession: total_size too large: " << totalSize_);
            totalChunks_ = 0;   // 使 Valid() == false
        }
    }

    bool UploadSession::Prepare() {
        if (!Valid() || !ops_) {
            CLEAT_LOGE(ErrDom::Job, ErrCode::InvalidParam,
                "UploadSession: invalid params or no ops, refuse to prepare");
            return false;
        }
        // 临时文件方案: 所有分块写入 <path>.uploading, 完成后原子替换目标(与旧实现一致)。
        // 仅清理上次残留的临时文件。
        if (ops_->FileExists(tmp_path_)) {
            if (!ops_->DeleteFile(tmp_path_)) {
                CLEAT_LOGE(ErrDom::Job, ErrCode::PlatformApiFailed,
                    "UploadSession: 无法清除残留临时文件, 拒绝启动: " << tmp_path_);
                return false;
            }
            LOG_DEBUG("UploadSession: 已清除残留临时文件 " << tmp_path_);
        }
        return true;
    }

    void UploadSession::BindRecord(std::shared_ptr<JobRecord> rec) {
        rec_ = std::move(rec);
        task_id_ = rec_ ? TaskIdOf(rec_->id) : std::string();
    }

    bool UploadSession::Finished() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return completed_ || failed_;
    }

    bool UploadSession::Completed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return completed_;
    }

    void UploadSession::AbortCleanup() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!completed_ && ops_ && ops_->FileExists(tmp_path_)) {
            if (ops_->DeleteFile(tmp_path_)) {
                LOG_DEBUG("UploadSession: deleted temp file: " << tmp_path_);
            }
            else {
                LOG_DEBUG("UploadSession: failed to delete temp file: " << tmp_path_);
            }
        }
    }

    void UploadSession::SendJson(const nlohmann::json& j) {
        JobRuntime::Emit(ToBytes(j));
    }

    void UploadSession::SendError(const std::string& message) {
        nlohmann::json err;
        err["cmd"] = Cmd::FileManager::UploadError();
        err["data"]["task_id"] = task_id_;
        err["data"]["path"] = path_;
        err["data"]["error"] = message;
        SendJson(err);
    }

    bool UploadSession::WriteChunk(int chunkIndex, const std::vector<uint8_t>& data) {
        // 已取消: 拒绝新块(与旧"not running, ignore chunk"同语义; 清理由 waiter 线程做)
        if (rec_ && !rec_->alive.load(std::memory_order_relaxed)) {
            LOG_WARN("UploadSession: cancelled, ignore chunk " << chunkIndex);
            return false;
        }

        // 范围校验
        if (chunkIndex < 0 || chunkIndex >= totalChunks_) {
            LOG_WARN("UploadSession: chunk index out of range: " << chunkIndex);
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);

        // 幂等: 已写入的块重复到达直接忽略
        if (chunkIndex <= lastWrittenChunk_) {
            LOG_DEBUG("UploadSession: duplicate chunk " << chunkIndex << ", ignore");
            return true;
        }
        // 顺序校验: 只接受下一块(server 按顺序发送)
        if (chunkIndex != lastWrittenChunk_ + 1) {
            LOG_WARN("UploadSession: out-of-order chunk " << chunkIndex
                << ", expected " << lastWrittenChunk_ + 1);
            return false;
        }

        if (!ops_->AppendFile(tmp_path_, data)) {
            CLEAT_LOGE(ErrDom::Job, ErrCode::PlatformApiFailed,
                "UploadSession: append failed, path=" << tmp_path_);
            // 清理不完整的临时文件, 原目标文件不受影响
            ops_->DeleteFile(tmp_path_);
            SendError("Write file failed");
            failed_ = true;
            if (rec_) JobRuntime::Finish(rec_->id, JobState::Failed);
            return false;
        }

        lastWrittenChunk_ = chunkIndex;

        // 完成判定: 写满所有块(不依赖"最后一块"标记, 防止缺失悬挂)
        if (lastWrittenChunk_ == totalChunks_ - 1) {
            // 原子替换: 临时文件 -> 目标文件(覆盖)
            if (!ops_->ReplaceFile(tmp_path_, path_)) {
                CLEAT_LOGE(ErrDom::Job, ErrCode::PlatformApiFailed,
                    "UploadSession: 替换目标文件失败, path=" << path_);
                ops_->DeleteFile(tmp_path_);
                SendError("Replace target file failed");
                failed_ = true;
                if (rec_) JobRuntime::Finish(rec_->id, JobState::Failed);
                return false;
            }
            completed_ = true;
            nlohmann::json resp;
            resp["cmd"] = Cmd::FileManager::UploadComplete();
            resp["data"]["task_id"] = task_id_;
            resp["data"]["message"] = "Upload complete";
            resp["data"]["path"] = path_;
            resp["data"]["total_size"] = totalSize_;
            SendJson(resp);
            LOG_INFO("UploadSession: complete, task=" << task_id_
                << ", path=" << path_ << ", size=" << totalSize_);
            if (rec_) JobRuntime::Finish(rec_->id, JobState::Completed);
        }
        return true;
    }

    void UploadWaiterJob(JobContext& ctx, std::shared_ptr<UploadSession> session) {
        LOG_DEBUG("UploadWaiterJob: start, task=task_" << ctx.id());
        while (ctx.Tick()) {
            if (session->Finished()) return;   // 终态已由会话上报(Finish), 直接退出
            // 取消/收尾 → SleepFor 立即醒(false) → 清理后退出(Killed)
            if (!ctx.SleepFor(std::chrono::milliseconds(100))) {
                session->AbortCleanup();
                return;
            }
        }
        session->AbortCleanup();   // Tick 因取消而 false: 未完成则清残留临时文件
    }

} // namespace cleat::Tasks
