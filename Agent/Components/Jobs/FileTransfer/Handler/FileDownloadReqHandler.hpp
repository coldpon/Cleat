#pragma once
#include "Core/Command/Handlers/IHandler.hpp"
#include "Common/Protocol/Packet.hpp"
#include "Core/Config/AgentConfig.hpp"
#include "Components/Jobs/FileTransfer/Provider/TransferJobs.hpp"
#include "Core/Job/JobRuntime.hpp"
#include "Components/Jobs/FileTransfer/Platform/IFileOps.hpp"  // 只依赖接口

namespace cleat {

    class FileDownloadReqHandler : public IHandler {
    public:
        ResourceKey ResourceOf(const CommandRequest& req) const override {
            const auto it = req.params.find("path");
            return (it != req.params.end() && it->is_string())
                ? MakeFileKey(it->get<std::string>()) : ResourceKey{};
        }
        nlohmann::json Execute(const CommandRequest& req) override {
            auto itPath = req.params.find("path");
            if (itPath == req.params.end() || !itPath->is_string()) {
                return Packet::Error("Missing 'path'");
            }
            std::string path = itPath->get<std::string>();

            // 默认分块从 AgentConfig 读(req.params 可覆盖); 历史硬编码 64KB
            int chunkSize = static_cast<int>(
                AgentConfig::Instance().GetServiceDefaults().download_chunk_size);
            auto itChunk = req.params.find("chunk_size");
            if (itChunk != req.params.end() && itChunk->is_number()) {
                chunkSize = itChunk->get<int>();
                if (chunkSize < 1024) chunkSize = 1024;
                if (chunkSize > 10 * 1024 * 1024) chunkSize = 10 * 1024 * 1024;
            }

            // 任务系统 Phase 2: 经唯一出生口派生下载 job。
            // 拒绝**可区分**( 修正): 低带宽不再折叠成空 task_id 假成功。
            const auto start = Tasks::SpawnDownloadJob(path, chunkSize);
            if (start.result == JobRuntime::TaskStart::Result::LowBandwidthRefused) {
                return Packet::Error("Low bandwidth channel, download refused", "LOW_BANDWIDTH");
            }
            if (!start.handle) {
                return Packet::Error("Download refused (agent stopping)", "REFUSED");
            }

            return Packet::Accepted({
                {"message", "Download started"},
                {"task_id", "task_" + std::to_string(start.handle.id())}
            });
        }
    };

} // namespace cleat