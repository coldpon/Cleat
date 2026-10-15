/**
 * @file JobStatusHandler.cpp
 * @brief JobStatus 命令处理器实现
 *
 * 只读查询: 按 id 精确一条; 按名取最新一条(终态历史保留, 附 matches 计数)。
 */

#include "JobStatusHandler.hpp"
#include "Core/Job/JobRuntime.hpp"
#include "Common/Protocol/Packet.hpp"

#include <string>

namespace cleat {

    nlohmann::json JobStatusHandler::Execute(const CommandRequest& req) {
        JobSnapshot s;
        bool found = false;
        nlohmann::json extra = nlohmann::json::object();

        if (const auto it_id = req.params.find("id");
            it_id != req.params.end() && it_id->is_number()) {
            const uint64_t id = it_id->get<uint64_t>();
            found = JobRuntime::StatusSnapshot(id, s);
            if (!found) {
                return Packet::Error("job not found: id=" + std::to_string(id), "NOT_FOUND");
            }
        }
        else if (const auto it_name = req.params.find("name");
            it_name != req.params.end() && it_name->is_string()) {
            const std::string name = it_name->get<std::string>();
            const auto matches = JobRuntime::FindByName(name);
            if (matches.empty()) {
                return Packet::Error("job not found: name=" + name, "NOT_FOUND");
            }
            s = matches.back();          // FindByName 按 id 升序 → back() = 最新
            found = true;
            extra["matches"] = matches.size();
        }
        else {
            return Packet::Error("missing 'id' or 'name' parameter");
        }

        nlohmann::json job = {
            {"id", s.id},
            {"name", s.name},
            {"kind", JobKindName(s.kind)},
            {"state", ToString(s.state)},
            {"age_ms", s.age_ms},
            {"timeout_ms", s.deadline_ms},
            {"resource", s.resource},
            {"finished", s.finished},
        };
        nlohmann::json out = {
            {"job", job},
            // 排水中: 已受理停止(或已超时)但线程**尚未真正退出** ——
            // 协作式 kill 不做强杀, 状态与消亡分开呈现, 不许假装即刻终止()
            {"draining", !s.finished &&
                (s.state == JobState::Stopping || s.state == JobState::TimedOut ||
                 !s.alive)},
        };
        for (const auto& [k, v] : extra.items()) out[k] = v;
        return Packet::Ok(std::move(out));
    }

} // namespace cleat
