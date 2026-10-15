#pragma once
/**
 * @file JobServiceResponses.hpp
 * @brief 服务启动/停止结果的统一响应映射(Phase 3; 14 个 Start/Stop Handler 共用)
 * @details 拒绝原因**可区分**( 服务半边): 旧 StartService 的 bool 折叠 4 种失败,
 *          handler 只能回一句 "X Service Start Failed!"; 现在按 TaskStart::Result
 *          给出带 code 的错误帧。成功响应的数据字段由各 handler 自带(线协议不变)。
 */

#include <string>

#include "Common/Protocol/Packet.hpp"
#include "Core/Job/JobRuntime.hpp"

namespace cleat {

    /// 启动结果 → 响应。fail_msg: StartFailed 时的消息(个别服务有更准确的文案, 如摄像头)。
    inline nlohmann::json ServiceStartResponse(const JobRuntime::TaskStart& r,
        const std::string& name, nlohmann::json ok_data, const std::string& fail_msg) {
        using R = JobRuntime::TaskStart::Result;
        switch (r.result) {
        case R::Started:
            return Packet::Ok(std::move(ok_data));
        case R::AlreadyRunning:
            return Packet::Error(name + " already running", "ALREADY_RUNNING");
        case R::LowBandwidthRefused:
            return Packet::Error("Low bandwidth channel, " + name + " refused", "LOW_BANDWIDTH");
        case R::HttpsRefused:
            return Packet::Error("https transport, streaming service " + name + " disabled",
                "HTTPS_FORBIDDEN");
        case R::NotFound:
            return Packet::Error(name + " not registered", "NOT_FOUND");
        case R::StartFailed:
        default:
            return Packet::Error(fail_msg);
        }
    }

} // namespace cleat
