/**
 * @file IModuleLoader.hpp
 * @brief 模块加载抽象接口
 * @details
 * 远程模块/内存加载的统一入口：反射加载 PE/DLL 到当前进程（不落盘）。
 * 具体实现由 Platform*_Loader 提供（CMake 平台过滤 + 工厂选择），Core 层零平台代码。
 */

#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cleat {

    /**
     * @brief 模块信息结构体
     */
    struct ModuleInfo {
        uint32_t type = 0;      ///< 模块类型：1 = PE/DLL（反射加载），2 = COFF/BOF（已落地 ：
                                ///  不走本接口/ModuleManager, 独立命令 BofExec —— 见 CoffLoader.hpp/
                                ///  PlatformWin_CoffLoader.cpp + BofHandler.hpp, 与"反射驻留模块"
                                ///  生命周期不同: BOF 是"执行即焚", 无需注册/卸载）
        uint32_t platform = 0;  ///< 目标平台：1 = x64 Windows，2 = x86 Windows，3 = Linux x64
        std::string module_id;  ///< 模块标识（用于日志和生命周期管理）
    };

    /**
     * @brief 已加载模块信息结构体
     * @details 包含内存映像基址和可用的导出函数列表
     */
    struct LoadedModule {
        void* base = nullptr;                 ///< 映像基址（反射加载，不在进程模块表中）
        std::vector<std::string> exports;     ///< 导出函数名称列表（用于命令注册）
        std::vector<std::pair<std::string, uintptr_t>> export_rvas;  ///< 导出名 → RVA（调用时使用 base + rva）
        std::string module_id;
    };

    /**
     * @brief 模块加载器抽象接口
     * @details 定义模块加载和卸载的抽象接口，具体实现由平台相关类提供。
     */
    class IModuleLoader {
    public:
        virtual ~IModuleLoader() = default;

        /**
         * @brief 将模块字节反射加载到当前进程
         * @param blob  模块原始字节数据（PE/DLL 或后续格式）
         * @param info  模块信息（类型、平台、标识等）
         * @param err   错误信息输出参数（失败时填充）
         * @return 已加载模块的智能指针，失败返回 nullptr
         * @details
         * 执行完整的反射加载流程：PE 头校验 → 分配映像内存 → 复制头和节区 → 设置节区权限 →
         * 重定位（处理基址偏移）→ IAT 绑定（导入表解析）→ TLS 回调 → 调用 DllMain(ATTACH) →
         * 枚举导出表。失败时通过 err 返回详细原因。
         * @note 契约(C10, ): 本函数为同步实现 —— 返回时 blob 的全部
         *       内容已复制进映像内存, 调用方可在返回后立即 SecureWipe(blob)。
         *       若未来改为异步(返回即加载未完成), 必须把擦除移入本函数内部,
         *       否则调用方提前擦除会导致加载读脏数据。
         */
        virtual std::unique_ptr<LoadedModule> Load(const std::vector<uint8_t>& blob,
            const ModuleInfo& info,
            std::string& err) = 0;

        /**
         * @brief 卸载已加载的模块
         * @param mod 要卸载的模块指针
         * @return 卸载是否成功
         * @details 调用 DllMain(DETACH) 后释放映像内存。
         *       可选导出 ModDeinit(若存在), 供模块停止自有线程 —— 模块线程不在
         *       引用计数保护范围内, 映像被清零/释放时仍在执行即崩溃。
         */
        virtual bool Unload(LoadedModule* mod) = 0;
    };

    /**
     * @brief 模块加载器工厂函数
     * @return 平台相关的模块加载器实例
     * @details 平台实现由 CMake 条件编译选择（与 INetworkOps 同模式）。
     */
    std::unique_ptr<IModuleLoader> CreateModuleLoader();

} // namespace cleat