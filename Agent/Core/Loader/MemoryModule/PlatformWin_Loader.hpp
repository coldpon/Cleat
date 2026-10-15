/**
 * @file PlatformWin_Loader.hpp
 * @brief Windows PE 反射加载器头文件（阶段 A）
 * @details
 * 将原始 DLL 字节反射加载进当前进程：段映射、重定位、IAT、TLS、DllMain、导出枚举。
 * 参考架构：pe_to_shellcode loader_v2（peloader.cpp） + sRDI Loader.cpp，自写实现。
 */

#pragma once
#include "Core/Loader/MemoryModule/IModuleLoader.hpp"

namespace cleat {

    /**
     * @brief Windows 平台 PE 反射加载器类
     * @details 实现 IModuleLoader 接口，提供 Windows PE/DLL 的反射加载能力。
     */
    class PlatformWinLoader : public IModuleLoader {
    public:
        PlatformWinLoader() = default;
        ~PlatformWinLoader() override = default;

        /**
         * @brief 反射加载 PE/DLL
         * @param blob 原始 DLL 字节
         * @param info 模块信息
         * @param err  错误信息输出
         * @return 加载后的模块指针，失败返回 nullptr
         */
        std::unique_ptr<LoadedModule> Load(const std::vector<uint8_t>& blob,
            const ModuleInfo& info,
            std::string& err) override;

        /**
         * @brief 卸载模块
         * @param mod 要卸载的模块
         * @return 卸载成功返回 true，失败返回 false
         */
        bool Unload(LoadedModule* mod) override;
    };

} // namespace cleat