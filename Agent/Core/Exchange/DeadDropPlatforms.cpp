/**
 * @file DeadDropPlatforms.cpp
 * @brief 平台注册表实现 + 内建平台登记
 */
#include "DeadDropPlatforms.hpp"

#include "Platform/GitHubIssue/GitHubIssueDeadDrop.hpp"
#include "Platform/Gist/GistDeadDrop.hpp"

#include "Common/Logger/Logger.hpp"

#include <algorithm>
#include <map>
#include <mutex>

namespace cleat {
namespace deaddrop {

    DeadDropPlatformRegistry& DeadDropPlatformRegistry::Instance() {
        static DeadDropPlatformRegistry instance;
        return instance;
    }

    void DeadDropPlatformRegistry::Register(const std::string& type, Factory factory) {
        if (type.empty() || !factory) {
            LOG_WARN("DeadDropPlatforms: 非法注册(type 空 / factory 空), 忽略");
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        factories_[type] = std::move(factory);
    }

    std::unique_ptr<IDeadDrop> DeadDropPlatformRegistry::Create(const std::string& type,
        IHttpOps& http, const std::string& url) const {
        Factory f;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = factories_.find(type);
            if (it == factories_.end()) return nullptr;
            f = it->second;
        }
        return f(http, url);
    }

    std::vector<std::string> DeadDropPlatformRegistry::KnownTypes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> out;
        out.reserve(factories_.size());
        for (const auto& kv : factories_) out.push_back(kv.first);
        std::sort(out.begin(), out.end());
        return out;
    }

    void RegisterBuiltinDeadDropPlatforms() {
        static const bool registered = [] {
            auto& r = DeadDropPlatformRegistry::Instance();
            r.Register("gist", [](IHttpOps& http, const std::string& url) {
                return std::make_unique<GistDeadDrop>(http, url);
                });
            r.Register("github_issues", [](IHttpOps& http, const std::string& url) {
                return std::make_unique<GitHubIssueDeadDrop>(http, url);
                });
            // 活投递平台(Telegram/Discord Webhook: Publish+Fetch 两面)待模式 2 排期
            return true;
            }();
        (void)registered;
    }

    std::unique_ptr<IDeadDrop> CreateDeadDropPlatform(const std::string& type,
        IHttpOps& http, const std::string& url) {
        RegisterBuiltinDeadDropPlatforms();
        return DeadDropPlatformRegistry::Instance().Create(type, http, url);
    }

} // namespace deaddrop
} // namespace cleat
