/**
 * @file InjectionRegistry.hpp
 * @brief 注入方法注册表: 方法名 → 实例(显式注册 + 查表)
 * @details
 * 与 DeadDropPlatformRegistry 同构(单例 + 互斥 + 同名覆盖)。内建方法的注册点在
 * RegisterBuiltin.cpp —— **加方法 = Methods/<名>/ 建目录 + 此处加一行 Register**,
 * 注册表本身零改动。
 *
 * 注册走**显式函数**而非静态自注册: 静态库下未引用目标文件会被链接器丢弃,
 * 自注册对象静默消失(DeadDropPlatforms.hpp / ChannelRegistry 同案裁定)。
 */
#pragma once

#include "Core/Inject/IInjectionMethod.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace cleat {
namespace injection {

    class InjectionRegistry {
    public:
        static InjectionRegistry& Instance();

        /// 注册方法(同名覆盖 = 测试可替换实现)
        void Register(std::unique_ptr<IInjectionMethod> method);

        /// 按名查表; 未知方法 = nullptr(handler 明确拒绝)
        IInjectionMethod* Find(const std::string& name) const;

        /// 已注册方法名(诊断/测试; map 序, 稳定)
        std::vector<std::string> KnownMethods() const;

    private:
        InjectionRegistry() = default;
        std::map<std::string, std::unique_ptr<IInjectionMethod>> methods_;
        mutable std::mutex mutex_;   ///< Register(装配期)与 Find(命令 worker)并发
    };

    /// 内建方法注册(幂等, 进程一次)—— 新方法在 RegisterBuiltin.cpp 加一行
    void RegisterBuiltinInjectionMethods();

} // namespace injection
} // namespace cleat
