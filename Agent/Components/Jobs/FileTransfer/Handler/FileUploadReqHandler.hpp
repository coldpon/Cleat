#pragma once
#include "Core/Command/Handlers/IHandler.hpp"
#include "Common/Protocol/Packet.hpp"
#include "Components/Jobs/FileTransfer/Provider/TransferJobs.hpp"
#include "Core/Job/JobRuntime.hpp"

namespace cleat {

    class FileUploadReqHandler : public IHandler {
    public:
        ResourceKey ResourceOf(const CommandRequest& req) const override {
            const auto it = req.params.find("path");
            return (it != req.params.end() && it->is_string())
                ? MakeFileKey(it->get<std::string>()) : ResourceKey{};
        }
        nlohmann::json Execute(const CommandRequest& req) override {
            auto itPath = req.params.find("path");
            auto itSize = req.params.find("total_size");
            auto itChunks = req.params.find("total_chunks");
            if (itPath == req.params.end() || !itPath->is_string() ||
                itSize == req.params.end() || !itSize->is_number() ||
                itChunks == req.params.end() || !itChunks->is_number()) {
                return Packet::Error("Missing required parameters: path, total_size, total_chunks");
            }
            std::string path = itPath->get<std::string>();
            uint64_t totalSize = itSize->get<uint64_t>();
            int totalChunks = itChunks->get<int>();

            // 任务系统 Phase 2: 上传 = 会话驱动 job(块仍由命令线程同步写,
            // 逐块 Ok/Error 响应语义不变); 平台不可用/参数非法/低带宽**可区分**( 修正)。
            std::shared_ptr<Tasks::IFileOps> ops(Tasks::CreateFileOps());
            if (!ops) {
                return Packet::Error("File upload not supported on this platform", "NOT_SUPPORTED");
            }
            auto session = std::make_shared<Tasks::UploadSession>(path, totalSize, totalChunks, ops);
            if (!session->Valid()) {
                return Packet::Error("total_size too large (limit 2GB)", "TOO_LARGE");
            }
            if (!session->Prepare()) {
                return Packet::Error("Cannot prepare upload (residual temp file)", "PREPARE_FAILED");
            }

            JobSpec spec;   // cleat::JobSpec(Core/Job; 注意不是 Tasks::)
            spec.name = "file-upload";
            spec.kind = JobKind::Task;
            spec.high_bandwidth = true;   // 大文件流式写入: 低带宽通道拒绝启动
            spec.attachment = session;
            const auto start = JobRuntime::StartTask(spec,
                [session](JobContext& ctx) { Tasks::UploadWaiterJob(ctx, session); });
            if (start.result == JobRuntime::TaskStart::Result::LowBandwidthRefused) {
                return Packet::Error("Low bandwidth channel, upload refused", "LOW_BANDWIDTH");
            }
            if (!start.handle) {
                return Packet::Error("Upload refused (agent stopping)", "REFUSED");
            }
            session->BindRecord(start.handle.record());   // 分块帧晚于本响应, 无竞态

            return Packet::Accepted({
                {"message", "Upload ready"},
                {"task_id", "task_" + std::to_string(start.handle.id())}
            });
        }
    };

} // namespace cleat