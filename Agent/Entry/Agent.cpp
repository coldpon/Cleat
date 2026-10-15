/**
 * @file Agent.cpp
 * @brief Agent主类实现
 * 
 * 实现Agent主类的启动和停止功能，作为AgentContext的轻量级包装器。
 */

#include "Agent.hpp"
#include "Composition/AgentContext.hpp"
#include "Core/Config/AgentConfig.hpp"

namespace cleat {

    Agent::Agent() : context_(std::make_unique<AgentContext>()) {
    }

    Agent::~Agent() = default; // AgentContext destructor runs Shutdown()

    /**
     * @brief 运行Agent
     * @return 运行状态码
     * 
     * 服务器地址从 AgentConfig 读取(编译期默认层, 不支持远程下发)
     */
    int Agent::Run() {
        const auto& cfg = AgentConfig::Instance();
        return context_->Run(cfg.server_host, cfg.server_port);
    }

    /**
     * @brief 停止Agent
     * 
     * 通过AgentContext停止整个Agent运行时。
     */
    void Agent::Stop() {
        context_->Stop();
    }

} // namespace cleat
