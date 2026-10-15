/**
 * @file SessionIdentity.hpp
 * @brief 被控端全局上下文（单例）
 * @details
 * 保存 Agent 运行期间的会话状态信息，包括：
 * - 会话 ID（session_id）：服务器分配的唯一会话标识
 * - 机器 ID（machine_id）：服务器分配的机器标识（首次连接时下发）
 * - 权限提升标志（elevated）：当前实例是否为提升权限版本
 * - 旧会话 ID（old_session_id）：自提权时旧实例的会话 ID
 * - 退出请求标志（exit_requested）：用于通知主循环优雅退出
 *
 * 所有成员变量的访问均通过互斥锁保护，保证线程安全。
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <mutex>

namespace cleat {

    /**
     * @brief 被控端全局上下文单例类
     * @details
     * 集中管理 Agent 的状态信息，提供线程安全的读写接口。
     * 主要用于会话管理、权限标识和退出控制。
     */
    class SessionIdentity {
    public:
        /**
         * @brief 获取单例实例
         * @return SessionIdentity 单例的引用
         */
        static SessionIdentity& Instance();

        // 禁止拷贝和移动
        SessionIdentity(const SessionIdentity&) = delete;
        SessionIdentity& operator=(const SessionIdentity&) = delete;

        /**
         * @brief 设置会话 ID
         * @param id 新的会话 ID
         * @details
         * 会话 ID 由服务器在首次连接确认时分配，此后保持不变。
         * 线程安全。
         */
        void SetSessionId(uint64_t id);

        /**
         * @brief 获取会话 ID
         * @return 当前会话 ID
         * @details 如果尚未设置，返回 0。
         */
        uint64_t GetSessionId() const;

        /**
         * @brief 设置机器 ID
         * @param id 机器唯一标识符
         * @details
         * 机器 ID 由服务器在 FirstConnect 响应时下发，用于标识同一台被控端。
         * 线程安全。
         */
        void SetMachineId(const std::string& id);

        /**
         * @brief 获取机器 ID
         * @return 当前机器 ID，可能为空字符串
         */
        std::string GetMachineId() const;

        /**
         * @brief 设置权限提升标志
         * @param elevated 是否为提升权限实例
         * @details
         * 该标志由命令行参数 --elevated 决定，表示当前进程是否以管理员/root 身份运行。
         * 线程安全。
         */
        void SetElevated(bool elevated);

        /**
         * @brief 检查当前实例是否具有提升权限
         * @return true 表示提升权限实例，false 表示普通实例
         */
        bool IsElevated() const;

        /**
         * @brief 设置旧实例的会话 ID
         * @param id 旧实例的会话 ID
         * @details
         * 用于自提权场景：当旧实例提升权限后重启新实例，旧实例需将自身会话 ID 告知新实例，
         * 以便服务器复用会话。
         * 线程安全。
         */
        void SetOldSessionId(uint64_t id);

        /**
         * @brief 获取旧实例的会话 ID
         * @return 旧实例的会话 ID，如果未设置则返回 0
         */
        uint64_t GetOldSessionId() const;

        /**
         * @brief 检查是否应该退出
         * @return true 表示请求退出，false 表示继续运行
         * @details 消费者只有两个：主循环（`SessionLoop`）与任务线程开工前检查（`CommandWorker`）。
         */
        bool ShouldExit() const;

        /// 单测专用：清除退出标志（与 ThreadGovernor::ResetForTests 配套；生产禁止调用）
        void ResetExitForTests();

    private:
        /// 退出写者收口（P1）：`RequestExit` 的唯一调用者 —— 见
        /// 《ThreadGovernor设计复盘与改造方案(第二版)》：把"退出只有一个写者"
        /// 从 grep 自觉变成**编译期约束**（以后谁再直接调就编不过）。
        friend class ThreadGovernor;

        /**
         * @brief 请求程序退出（**private**：外部一律走 `ThreadGovernor::RequestExit()`）
         * @details 设置退出标志，主循环定期检查并在适当时机退出进程。
         *          `ThreadGovernor::RequestExit()` 会在调用本函数前先置门闸终态 ——
         *          identity-only 写点，门闸收不到）。
         */
        void RequestExit();

        SessionIdentity() = default;
        ~SessionIdentity() = default;

        mutable std::mutex mutex_;              ///< 保护共享数据的互斥锁
        uint64_t session_id_ = 0;               ///< 当前会话 ID
        std::string machine_id_;                ///< 机器唯一标识符
        bool elevated_ = false;                 ///< 是否具有提升权限
        uint64_t old_session_id_ = 0;           ///< 旧实例的会话 ID（自提权时使用）
        std::atomic<bool> exit_requested_{ false }; ///< 退出请求标志（原子操作）
    };

} // namespace cleat