/**
 * @file JobKillHandler.cpp
 * @brief JobKill 命令处理器实现
 *
 * 协作式停止(): 只受理(置取消令牌 + 唤醒 + Running→Stopping), 不强杀。
 * 按名歧义(多个同名在跑)拒绝并提示用 id。
 */

#include "JobKillHandler.hpp"
#include "Core/Job/JobRuntime.hpp"
#include "Common/Protocol/Packet.hpp"

#include <string>

namespace cleat {

    nlohmann::json JobKillHandler::Execute(const CommandRequest& req) {
        if (const auto it = req.params.find("id");
            it != req.params.end() && it->is_number()) {
            const uint64_t id = it->get<uint64_t>();
            if (!JobRuntime::Cancel(id)) {
                return Packet::Error("cannot kill job: id=" + std::to_string(id) +
                    " (not found or already finished)", "KILL_REFUSED");
            }
            return Packet::Ok({ {"id", id}, {"state", "Stopping"},
                {"message", "stop accepted (cooperative; see JobStatus for draining)"} });
        }
        if (const auto it = req.params.find("name");
            it != req.params.end() && it->is_string()) {
            const std::string name = it->get<std::string>();
            const int rc = JobRuntime::CancelByName(name);
            if (rc == 0) {
                return Packet::Error("cannot kill job: name=" + name +
                    " (not found or already finished)", "KILL_REFUSED");
            }
            if (rc < 0) {
                return Packet::Error("ambiguous name: " + name +
                    " (multiple running jobs; kill by id)", "AMBIGUOUS_NAME");
            }
            return Packet::Ok({ {"name", name}, {"state", "Stopping"},
                {"message", "stop accepted (cooperative; see JobStatus for draining)"} });
        }
        return Packet::Error("missing 'id' or 'name' parameter");
    }

} // namespace cleat
