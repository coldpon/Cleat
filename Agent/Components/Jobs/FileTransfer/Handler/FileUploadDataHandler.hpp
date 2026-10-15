#pragma once
#include "Core/Command/Handlers/IHandler.hpp"
#include "Common/Protocol/Packet.hpp"
#include "Components/Jobs/FileTransfer/Provider/TransferJobs.hpp"
#include "Core/Job/JobRuntime.hpp"

#include <memory>
#include <stdexcept>

namespace cleat {

    // 上传数据块：数据随混合帧二进制体到达（req.payload），
    // 不再使用 Base64。JSON 头含 task_id / chunk_index。
    class FileUploadDataHandler : public IHandler {
    public:
        ResourceKey ResourceOf(const CommandRequest& req) const override {
            const auto it = req.params.find("task_id");
            return (it != req.params.end() && it->is_string())
                ? ResourceKey{ ResourceKind::File, "upload_task:" + it->get<std::string>() }
                : ResourceKey{};
        }
        nlohmann::json Execute(const CommandRequest& req) override {
            auto itTask = req.params.find("task_id");
            auto itChunk = req.params.find("chunk_index");
            if (itTask == req.params.end() || !itTask->is_string() ||
                itChunk == req.params.end() || !itChunk->is_number()) {
                return Packet::Error("Missing required parameters: task_id, chunk_index");
            }
            std::string task_id = itTask->get<std::string>();
            int chunkIndex = itChunk->get<int>();

            if (req.payload.empty()) {
                return Packet::Error("Missing binary payload");
            }

            // 任务系统 Phase 2: task_id = "task_<N>", N 即 JobRecord id;
            // 会话对象挂在记录附件上(单一来源, 取代 TaskManager::GetTask 的第二份 map)。
            if (task_id.rfind("task_", 0) != 0) {
                return Packet::Error("Invalid task_id format");
            }
            uint64_t job_id = 0;
            try {
                job_id = std::stoull(task_id.substr(5));
            }
            catch (const std::exception&) {
                return Packet::Error("Invalid task_id format");
            }
            auto session = std::static_pointer_cast<Tasks::UploadSession>(
                JobRuntime::AttachmentOf(job_id));
            if (!session) {
                return Packet::Error("Task not found or invalid type");
            }
            if (!session->WriteChunk(chunkIndex, req.payload)) {
                return Packet::Error("Failed to write chunk");
            }
            return Packet::Ok({{ "chunk_index", chunkIndex }, { "message", "Chunk written" }});
        }
    };

} // namespace cleat
