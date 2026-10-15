/**
 * @file Agent.hpp
 * @brief Agent主类（轻量级外壳）
 * 
 * AgentContext的轻量级外壳包装器，拥有完整的运行时图。
 * 添加新功能不再需要修改此类。
 */

#pragma once
#include <memory>

namespace cleat {

    class AgentContext;

    /**
     * @brief Agent主类（轻量级外壳）
     * 
     * 作为AgentContext的轻量级包装器，提供简单的启动和停止接口。
     * 服务器地址/端口配置统一从AgentConfig读取，避免硬编码。
     */
    class Agent {
    public:
        Agent();
        ~Agent();

        Agent(const Agent&) = delete;
        Agent& operator=(const Agent&) = delete;

        /**
         * @brief 运行Agent
         * @return 运行状态码
         */
        int Run();

        /**
         * @brief 停止Agent
         */
        void Stop();

    private:
        // 服务器地址/端口不再硬编码于此, 统一从 AgentConfig 读取(编译期默认层)
        std::unique_ptr<AgentContext> context_;
    };

} // namespace cleat
