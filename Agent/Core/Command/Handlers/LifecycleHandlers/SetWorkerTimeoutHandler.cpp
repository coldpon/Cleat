/**
 * @file SetWorkerTimeoutHandler.cpp
 * @brief 工作超时设置命令处理器实现
 * 
 * 实现命令执行超时时间的动态配置功能。
 */

#include "SetWorkerTimeoutHandler.hpp"
#include "Core/Command/Queue/CommandWorker.hpp"
#include "Core/Config/JsonField.hpp"
#include "Common/Logger/Logger.hpp"
#include "Common/Protocol/Packet.hpp"
#include <cstdint>

namespace cleat {

    /**
     * @brief 执行工作超时设置命令
     * @param req.params 配置参数JSON对象
     * @param req.payload 可选的二进制数据（当前未使用）
     * @return 响应JSON（包含设置的超时时间）
     * 
     * 此方法从JSON参数中提取timeout_ms字段，并设置到CommandWorker中。
     * 超时时间以毫秒为单位，0表示不限制超时时间。
     * 
     * @note 参数验证：
     * - 必须包含timeout_ms字段
     * - timeout_ms必须是数值类型
     * - 设置成功后会立即生效，影响后续所有命令执行
     */
    nlohmann::json SetWorkerTimeoutHandler::Execute(const CommandRequest& req) {
        // 同族(第十七轮): 旧写法 is_number() 对 float 亦为真 → get<uint32_t>()
        // 的越界 double→整型转换是 UB(下发 3e9 即触发)。统一走 JsonField 整型读取。
        uint32_t ms = 0;
        if (!detail::ReadUInt(req.params, "timeout_ms", ms)) {
            return Packet::Error("SetWorkerTimeout: 缺少或无效的 timeout_ms 参数(须为非负整数)");
        }
        CommandWorker::Instance().SetTaskTimeoutMs(ms);
        return Packet::Ok({{ "timeout_ms", ms }});
    }

} // namespace cleat
