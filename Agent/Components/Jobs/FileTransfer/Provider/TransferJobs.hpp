#pragma once
/**
 * @file TransferJobs.hpp
 * @brief 文件传输的 job 化
 * @details
 * 旧 DownloadTask/UploadTask(自持线程四件套 + ITask)退化为两种形状:
 *   * **下载 = 循环驱动 job**: `RunDownloadJob` 是可测试核心(注入 IFileOps),
 *     生产包装 `SpawnDownloadJob` 自建平台 ops —— 分块粒度暂停/取消/收尾全部经
 *     JobContext(出生自带门闸), 不再有任何 GovLoop 接线。
 *   * **上传 = 会话驱动 job**: `UploadSession` 由 handler 命令线程**同步**写块
 *     (与旧 UploadTask 相同 —— 分块的 Ok/Error 响应必须逐块如实), job 线程只是
 *     会话的**生命周期载体**(等待完成/取消/收尾并做清理); 终态由会话经
 *     `JobRuntime::Finish` 上报(它知道事实), 线程退出的自动落终态让位(终态优先)。
 *
 * **线协议零改动**: 帧 cmd/字段/分块序与旧实现逐字一致; task_id 仍为 "task_<N>"
 * (N = JobRecord id, 单调递增, 服务端透传不解析)。
 *
 * **顺带修正的已登记缺陷**(行为改进, 提交信息另行登记):
 *   * N4: 发送失败不再"记账照走" —— 同一块可打断重试(200ms 片), 只记成功的块;
 *   * L0-2(复核重开): 平台 ops 为空(Linux 未实现)在入口判空报错, 不再 SIGSEGV;
 *   * N20(部分): 低带宽拒绝/参数拒绝返回**可区分错误**, 不再折叠成空 task_id 假成功。
 */

#include <memory>
#include <string>
#include <vector>

#include "Common/ThirdParty/json.hpp"
#include "Components/Jobs/FileTransfer/Platform/IFileOps.hpp"
#include "Core/Job/JobContext.hpp"
#include "Core/Job/JobRuntime.hpp"

namespace cleat::Tasks {

    /**
     * @brief 下载循环(可测试核心; 生产包装见 SpawnDownloadJob)
     * @param ctx    job 上下文(门闸/取消/出站)
     * @param path   目标文件路径
     * @param chunkSize 分块大小(字节; handler 已钳制)
     * @param ops    平台文件操作(由调用方注入; 为空的行为在包装层判)
     * @return 正常返回 = job 线程退出(终态由 RunJob/Finish 落)
     */
    void RunDownloadJob(JobContext& ctx, const std::string& path, int chunkSize, IFileOps& ops);

    /**
     * @brief 下载的生产出生口: 自建平台 ops + 判空 + StartTask。
     * @return TaskStart::Started 时 handle 有效(task_id = "task_" + id)。
     */
    JobRuntime::TaskStart SpawnDownloadJob(const std::string& path, int chunkSize);

    /**
     * @brief 上传会话(旧 UploadTask 的内核; 无自有线程, 块由 handler 命令线程同步写)
     * @details 线程安全: 全部可变状态在 mutex_ 下; 取消令牌来自绑定的 JobRecord
     *          (alive 原子, 读侧无锁)。
     */
    class UploadSession {
    public:
        /// ops 由调用方注入(handler 先判平台可用性; 测试注入 FakeFileOps) ——
        /// 会话自身不再 CreateFileOps(否则注入失效, 与调用方各持一份平台实现)
        UploadSession(std::string path, uint64_t totalSize, int totalChunks,
            std::shared_ptr<IFileOps> ops);

        /// 参数防御(2GB 上限 + 块数有效) —— 旧构造器的静默失效改为显式判定
        bool Valid() const { return totalChunks_ > 0; }

        /// 旧 Start() 语义: 清理上次残留的临时文件; 失败 = 拒绝启动
        bool Prepare();

        /// 绑定所属 job 记录(StartTask 成功后、响应返回前调用; 分块帧晚于响应, 无竞态)
        void BindRecord(std::shared_ptr<JobRecord> rec);

        /**
         * @brief 写入一个数据块(与旧 UploadTask::WriteChunk 同语义/同返回值)。
         * @return true = 已写入(或重复块幂等忽略); false = 拒绝(乱序/越界/已取消/写失败)。
         */
        bool WriteChunk(int chunkIndex, const std::vector<uint8_t>& data);

        /// 会话是否已到终态(完成或失败; waiter 线程据此退出)
        bool Finished() const;
        bool Completed() const;

        /// 取消/收尾路径的清理: 未完成时删除残留临时文件(旧 Stop() 语义)
        void AbortCleanup();

    private:
        void SendJson(const nlohmann::json& j);
        void SendError(const std::string& message);

        std::string path_;
        std::string tmp_path_;     ///< 上传临时文件: 全部块写完后再原子替换目标
        uint64_t totalSize_;
        int totalChunks_;
        std::shared_ptr<IFileOps> ops_;
        std::shared_ptr<JobRecord> rec_;   ///< 所属 job(取消令牌 + id; BindRecord 后有效)
        std::string task_id_;              ///< "task_<N>"; BindRecord 时从记录派生

        mutable std::mutex mutex_;
        int lastWrittenChunk_ = -1;
        bool completed_ = false;
        bool failed_ = false;
    };

    /**
     * @brief 上传 job 的载体线程函数: 等会话到终态 / 被取消 / 收尾, 并做清理。
     * @details 会话的完成发生在命令线程(WriteChunk), 本线程只负责" job 生命周期":
     *          会话完成 → 自然退出(Finish 已落终态, 自动落终态让位); 取消/收尾 →
     *          AbortCleanup 后退出(Killed)。
     */
    void UploadWaiterJob(JobContext& ctx, std::shared_ptr<UploadSession> session);

} // namespace cleat::Tasks
