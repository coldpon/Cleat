/**
 * @file ModuleLoaderFactory.cpp
 * @brief 模块加载器工厂实现
 * @details 根据当前平台选择对应的加载器实现，与 NetworkOpsFactory 同模式。
 */

#include "Core/Loader/MemoryModule/IModuleLoader.hpp"

#ifdef _WIN32
#include "Core/Loader/MemoryModule/PlatformWin_Loader.hpp"
#elif __linux__
 // Linux 反射加载器（阶段 A 暂不实现，接口已预留；待后续接入）
#else
#error "Unsupported platform"
#endif

namespace cleat {

    /**
     * @brief 创建平台相关的模块加载器实例
     * @return 加载器智能指针，当前 Linux 下返回 nullptr（阶段 A 仅支持 Windows）
     */
    std::unique_ptr<IModuleLoader> CreateModuleLoader() {
#ifdef _WIN32
        return std::make_unique<PlatformWinLoader>();
#elif __linux__
        return nullptr;   // 阶段 A 仅 Windows
#else
#error "Unsupported platform"
#endif
    }

} // namespace cleat