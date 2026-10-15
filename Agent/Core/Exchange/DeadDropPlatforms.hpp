/**
 * @file DeadDropPlatforms.hpp
 * @brief 死投递平台注册表: 类型 → 适配器工厂(配置驱动注册的查表点)
 * @details
 * **加平台三步**:
 *   1. 建 `Core/Exchange/Platform/<名>/` 目录, 写适配器(实现 IDeadDrop);
 *   2. 在 RegisterBuiltinDeadDropPlatforms() 加一行 Register;
 *   3. 配置 recovery.platforms[] 加 {"type":"<名>","url":...}。
 *
 * 注册走**显式函数**而非静态自注册: 静态库下未引用目标文件会被链接器丢弃,
 * 自注册对象静默消失 —— 与 ChannelRegistry::RegisterBuiltinChannels 同一裁定。
 *
 * Factory 形参 = IHttpOps&(通道层调用者) + url(平台内容地址)。平台特有参数
 * (如未来的 token/频道名)进 PlatformSpec 扩展字段后再加形参, 不走字符串约定。
 */
#pragma once

#include "Core/Exchange/IDeadDrop.hpp"
#include "Core/Comms/Platform/IHttpOps.hpp"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace cleat {
namespace deaddrop {

    class DeadDropPlatformRegistry {
    public:
        using Factory = std::function<std::unique_ptr<IDeadDrop>(
            IHttpOps& http, const std::string& url)>;

        static DeadDropPlatformRegistry& Instance();

        /// 注册平台工厂(type 小写; 同名覆盖 = 测试可替换实现)
        void Register(const std::string& type, Factory factory);

        /// 按 type 构造适配器; 未知类型 = nullptr(装配层跳过并告警)
        std::unique_ptr<IDeadDrop> Create(const std::string& type,
            IHttpOps& http, const std::string& url) const;

        /// 已注册类型(诊断/测试)
        std::vector<std::string> KnownTypes() const;

    private:
        DeadDropPlatformRegistry() = default;
        std::map<std::string, Factory> factories_;
        mutable std::mutex mutex_;   ///< Register(装配期)与 Create(恢复线程)并发
    };

    /// 内建平台注册(幂等; 进程一次)—— 新平台在此加一行
    void RegisterBuiltinDeadDropPlatforms();

    /// 装配层便捷入口: 确保内建已注册后按 type 构造(未知类型 = nullptr)
    std::unique_ptr<IDeadDrop> CreateDeadDropPlatform(const std::string& type,
        IHttpOps& http, const std::string& url);

} // namespace deaddrop
} // namespace cleat
