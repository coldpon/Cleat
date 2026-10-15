/**
 * @file SetConfigHandler.cpp
 * @brief 配置设置命令处理器实现
 * 
 * 实现Agent运行参数的动态配置和更新功能。
 */

#include "SetConfigHandler.hpp"
#include "Core/Config/AgentConfig.hpp"
#include "Core/Command/Queue/CommandWorker.hpp"
#include "Common/Logger/Logger.hpp"
#include "Common/Protocol/Packet.hpp"

namespace cleat {

    /**
     * @brief 执行配置设置命令
     * @param req.params 配置参数JSON对象
     * @param req.payload 可选的二进制数据（当前未使用）
     * @return 响应JSON（包含已应用的配置）
     * 
     * 此方法将接收到的配置参数应用到AgentConfig单例，
     * 并立即生效的关键参数（如命令超时、日志级别）会联动更新。
     * 
     * @note 联动生效的配置：
     * - worker_timeout_ms: 命令工作超时立即生效
     * - log_level: 日志级别立即生效（本地输出和DEBUG宏过滤）
     * 其他配置字段由各组件在每次使用时读取，天然即时生效。
     */
    nlohmann::json SetConfigHandler::Execute(const CommandRequest& req) {
        AgentConfig& cfg = AgentConfig::Instance();
        nlohmann::json applied = cfg.ApplyJson(req.params);

        // 联动: 命令超时立即生效(其余字段由各组件每次使用时读取, 天然即时)
        CommandWorker::Instance().SetTaskTimeoutMs(cfg.GetWorkerTimeoutMs());
        // 联动: 日志级别 —— **仅当下发内容真的携带(且被应用了) log_level 时才同步**
        // ( 选项 A, )。原实现无条件 `SetLevel(cfg.GetLogLevel())`: 而
        // ApplyJson 只在参数真携带合法 log_level 时才改配置值, 于是"只推其他域的
        // SetConfig"会把 Logger 拉回**配置默认**(1=INFO), 覆盖掉启动时由 `AgentEntry`
        // 施加的运行时覆盖(DEBUG) → 该进程此后 DEBUG 全消失。后果落在**诊断可见性**
        // 上: 实测据此误判过"换链后重新注册没走状态机"(其实是轨迹被抑制)。
        //  散键归位: log_level 住进 runtime 域; runtime 域回显=**本次实际
        // 应用字段**(非整域回显, 语义见 ApplyJson 头注释) —— applied.runtime 含
        // log_level 即本次真带了合法 log_level, 门控信号与散键时代严格等价。
        const auto itRt = applied.find("runtime");
        if (itRt != applied.end() && itRt->is_object() && itRt->contains("log_level")) {
            Logger::Instance().SetLevel(static_cast<cleat::LogLevel>(cfg.GetLogLevel()));
        }

        if (applied.empty()) {
            return Packet::Error("SetConfig: Unknown field");
        }
        // 打印**运行时**级别(诊断关心的就是这个), 并标明配置值 —— 二者可能不同(N30)
        LOG_INFO("SetConfig: 配置已更新, 运行时 log_level="
            << static_cast<int>(Logger::Instance().GetLevel())
            << "(配置值 " << cfg.GetLogLevel() << ") applied=" << applied.dump());
        return Packet::Ok({{ "applied", std::move(applied) }});
    }

} // namespace cleat
