/**
 * @file JobListHandler.cpp
 * @brief JobList 命令处理器实现
 *
 * 只读聚合: JobRuntime 快照过滤 + ThreadGovernor 名册对账。
 * 过滤/序列化/对账均为本文件内部实现细节, 不进类契约(头文件)。
 */

#include "JobListHandler.hpp"
#include "Core/Concurrency/ThreadGovernor.hpp"
#include "Core/Job/JobRuntime.hpp"
#include "Common/Logger/Logger.hpp"
#include "Common/Protocol/Packet.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <string>

namespace cleat {

    namespace {

        std::string ToLower(std::string s) {
            std::transform(s.begin(), s.end(), s.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        }

        bool KindMatches(JobKind k, const std::string& filter) {
            return ToLower(JobKindName(k)) == ToLower(filter);
        }

        bool StateMatches(JobState s, const std::string& filter) {
            // 粗分类(面板常用): running(Starting/Running/Stopping) / finished(全部终态);
            // 也接受精确状态名(如 "TimedOut")
            const std::string f = ToLower(filter);
            if (f == "running" || f == "active") {
                return s == JobState::Starting || s == JobState::Running || s == JobState::Stopping;
            }
            if (f == "finished" || f == "done" || f == "terminal") {
                return s == JobState::Completed || s == JobState::Killed ||
                       s == JobState::TimedOut || s == JobState::Failed;
            }
            return ToLower(ToString(s)) == f;
        }

        nlohmann::json SnapshotToJson(const JobSnapshot& s) {
            return {
                {"id", s.id},
                {"name", s.name},
                {"kind", JobKindName(s.kind)},
                {"state", ToString(s.state)},
                {"age_ms", s.age_ms},
                {"timeout_ms", s.deadline_ms},
                {"resource", s.resource},
                {"finished", s.finished},
            };
        }

        /// 对账: 登记表中 kind ∈ {Service, Task} 且未终结的记录, 必须在名册里查得到名字。
        /// 命令类有意不进名册(), 不参与对账; Infrastructure/Main 属框架, 不参与。
        nlohmann::json RosterMismatch() {
            std::set<std::string> roster_names;
            for (const UnitSnapshot& u : ThreadGovernor::List()) {
                roster_names.insert(u.name);
            }
            nlohmann::json arr = nlohmann::json::array();
            for (const JobSnapshot& s : JobRuntime::List()) {
                if (s.kind != JobKind::Service && s.kind != JobKind::Task) continue;
                if (s.finished) continue;
                if (roster_names.count(s.name)) continue;
                LOG_WARN("JobList: 对账差集 —— job '" << s.name
                    << "'(#" << s.id << ") 在登记表但不在线程名册(疑似漏网)");
                arr.push_back(SnapshotToJson(s));
            }
            return arr;
        }

    } // namespace

    nlohmann::json JobListHandler::Execute(const CommandRequest& req) {
        std::string kind_filter;
        std::string state_filter;
        if (const auto it = req.params.find("kind"); it != req.params.end() && it->is_string()) {
            kind_filter = it->get<std::string>();
        }
        if (const auto it = req.params.find("state"); it != req.params.end() && it->is_string()) {
            state_filter = it->get<std::string>();
        }

        const auto jobs = JobRuntime::List();
        nlohmann::json arr = nlohmann::json::array();
        for (const JobSnapshot& s : jobs) {
            if (!kind_filter.empty() && !KindMatches(s.kind, kind_filter)) continue;
            if (!state_filter.empty() && !StateMatches(s.state, state_filter)) continue;
            arr.push_back(SnapshotToJson(s));
        }

        return Packet::Ok({ {"jobs", arr}, {"roster_mismatch", RosterMismatch()} });
    }

} // namespace cleat
