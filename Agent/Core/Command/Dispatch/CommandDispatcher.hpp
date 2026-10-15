#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "Core/Command/Handlers/IHandler.hpp"
#include "Common/ThirdParty/json.hpp"

namespace cleat {

    /**
     * @brief 命令分发器（线程安全）
     *
     * - 持有命令名 -> 处理器的映射表。
     * - 提供注册和调度方法。
     * - Execute 捕获所有异常，返回错误 JSON，绝不逃逸。
     */
    class CommandDispatcher {
    public:
        using HandlerPtr = std::unique_ptr<IHandler>;
        using HandlerFactory = std::function<HandlerPtr()>;

        CommandDispatcher() = default;
        ~CommandDispatcher() = default;

        // 禁止拷贝/移动（因含有互斥量）
        CommandDispatcher(const CommandDispatcher&) = delete;
        CommandDispatcher& operator=(const CommandDispatcher&) = delete;
        CommandDispatcher(CommandDispatcher&&) = delete;
        CommandDispatcher& operator=(CommandDispatcher&&) = delete;

        /// 未显式声明优先级 → CommandProcessor 回退到内置分类表(fallbackPriority)
        static constexpr uint8_t kPriorityUnset = 255;

        /**
         * @brief 注册命令处理器（工厂方式）
         * @param cmd 命令名称（如 "Ping"）
         * @param factory 返回 unique_ptr<IHandler> 的工厂函数
         * @param priority 调度优先级(0=最高, 4=最低; 缺省 kPriorityUnset 表示
         *                 未声明, 由 CommandProcessor 按内置分类表回退)
         * @details 优先级随注册走 —— 消灭 CommandProcessor 内第二份硬编码
         *          字符串表(漏注册=调度顺序不优)。生存命令(RegisterCore)
         *          显式声明 0。
         */
        void RegisterHandler(const std::string& cmd, HandlerFactory factory,
            uint8_t priority = kPriorityUnset);

        /// 查询命令显式声明的优先级(kPriorityUnset = 未声明)
        uint8_t PriorityOf(const std::string& cmd) const;

        /// 单条注册命令的自述快照(AgentDescribe 机器可读自述用)
        struct CommandInfo {
            std::string cmd;
            uint8_t priority = kPriorityUnset;  ///< 显式声明的调度优先级
        };

        /**
         * @brief 枚举当前注册的全部命令(锁内快照, 按 cmd 字典序)
         * @details 反映**活注册表**而非编译期静态表: CLEAT_MINIMAL 裁剪、模块
         *          动态 Load/Unload 的结果自动体现在清单里 —— 自述与实际
         *          可调用面严格一致。
         */
        std::vector<CommandInfo> ListHandlers() const;

        // 注销命令处理器(模块卸载时调用; 不存在的命令静默忽略)
        void UnregisterHandler(const std::string& cmd);

        /**
         * @brief 派发命令
         * @param cmd 命令名称
         * @param params JSON 参数
         * @param req 请求对象
         * @return JSON 响应（始终为有效 JSON）
         * @note 内部捕获所有异常，返回 {"status":"error","message":"..."}
         */
        nlohmann::json Dispatch(const std::string& cmd, const CommandRequest& req);

        /**
         * @brief 查询命令的目标资源: 需要实例化一次 handler 才能问到
         * @details 资源声明是**实例**上的虚函数(`IHandler::ResourceOf`), 故这里经工厂创建一次
         *          再询问 —— 在树 handler 的构造都很轻(只存 provider 引用), 代价可忽略。
         *          投递方据此在**入队时**就带上资源 key, 执行角色才知道要占什么。
         * @return 未注册命令返回空 key(不参与互斥)
         */
        ResourceKey ResourceOf(const std::string& cmd, const CommandRequest& req) const;

    private:
        std::unordered_map<std::string, HandlerFactory> handlers_;
        std::unordered_map<std::string, uint8_t> priorities_;  ///< 显式优先级(cmd → 0-4)
        mutable std::mutex mutex_;
    };

} // namespace cleat