/**
 * @file SessionIdentity.cpp
 * @brief 被控端全局上下文实现
 * @details
 * 实现 SessionIdentity 单例模式，提供线程安全的会话状态管理接口。
 */

#include "SessionIdentity.hpp"

namespace cleat {

    /**
     * @brief 获取 SessionIdentity 单例实例
     * @return SessionIdentity 单例的引用
     * @details
     * 使用 C++11 的静态局部变量实现线程安全的懒加载单例模式。
     */
    SessionIdentity& SessionIdentity::Instance() {
        static SessionIdentity instance;
        return instance;
    }

    /**
     * @brief 设置会话 ID
     * @param id 新的会话 ID
     * @details
     * 加锁保护，确保并发写入安全。
     */
    void SessionIdentity::SetSessionId(uint64_t id) {
        std::lock_guard<std::mutex> lock(mutex_);
        session_id_ = id;
    }

    /**
     * @brief 获取会话 ID
     * @return 当前会话 ID
     * @details
     * 加锁保护，确保读取到最新的值。
     */
    uint64_t SessionIdentity::GetSessionId() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return session_id_;
    }

    /**
     * @brief 设置机器 ID
     * @param id 机器唯一标识符
     * @details
     * 加锁保护，确保并发写入安全。机器 ID 由服务器在 FirstConnect 响应时设置。
     */
    void SessionIdentity::SetMachineId(const std::string& id) {
        std::lock_guard<std::mutex> lock(mutex_);
        machine_id_ = id;
    }

    /**
     * @brief 获取机器 ID
     * @return 当前机器 ID
     * @details
     * 加锁保护，确保读取到最新的值。
     */
    std::string SessionIdentity::GetMachineId() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return machine_id_;
    }

    /**
     * @brief 设置权限提升标志
     * @param elevated 是否为提升权限实例
     * @details
     * 加锁保护，确保并发写入安全。该标志通常在启动时根据命令行参数设置。
     */
    void SessionIdentity::SetElevated(bool elevated) {
        std::lock_guard<std::mutex> lock(mutex_);
        elevated_ = elevated;
    }

    /**
     * @brief 检查当前实例是否具有提升权限
     * @return true 表示提升权限，false 表示普通权限
     * @details
     * 加锁保护，确保读取到最新的值。
     */
    bool SessionIdentity::IsElevated() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return elevated_;
    }

    /**
     * @brief 设置旧实例的会话 ID
     * @param id 旧实例的会话 ID
     * @details
     * 用于自提权场景：新实例通过命令行参数接收旧会话 ID，以便服务器复用会话。
     * 加锁保护。
     */
    void SessionIdentity::SetOldSessionId(uint64_t id) {
        std::lock_guard<std::mutex> lock(mutex_);
        old_session_id_ = id;
    }

    /**
     * @brief 获取旧实例的会话 ID
     * @return 旧实例的会话 ID，如果未设置则返回 0
     * @details
     * 加锁保护，确保读取到最新的值。
     */
    uint64_t SessionIdentity::GetOldSessionId() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return old_session_id_;
    }

    /**
     * @brief 请求程序退出
     * @details
     * 原子设置退出标志，主循环可轮询 ShouldExit() 判断是否退出。
     * 通常由 ExitProgram 命令触发。
     */
    void SessionIdentity::RequestExit() {
        exit_requested_.store(true);
    }

    /**
     * @brief 检查是否请求退出
     * @return true 表示已请求退出，false 表示继续运行
     * @details
     * 原子读取退出标志，主循环在每次迭代中调用此函数。
     */
    bool SessionIdentity::ShouldExit() const {
        return exit_requested_.load();
    }

    /**
     * @brief 单测专用：清除退出请求标志
     * @details
     * 测试单进程共享本单例 —— ThreadGovernor::RequestExit 的并轨设计会把退出
     * 标志带进后续用例(CommandWorker 任务线程开工前的 ShouldExit 检查因此被短路)。
     * 由 ThreadGovernor::ResetForTests 镜像调用(并轨建立耦合, 复位亦镜像)；
     * 生产代码禁止调用。
     */
    void SessionIdentity::ResetExitForTests() {
        exit_requested_.store(false);
    }

} // namespace cleat