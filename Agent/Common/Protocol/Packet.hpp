#pragma once
// 统一报文构造(协议语义层, 与 Resp.hpp 同级):
//   响应: {"status":..., "data":...} / {"status":"error","message":...,"code":...}
//   推送: {"cmd":..., "data":...}                        (A→S 主动上报)
// 分层约定: Common 不依赖 Core/模块; 传输层帧格式在 Core/Comms/Internal,
// 这里只负责 JSON 载荷语义
#include "Common/ThirdParty/json.hpp"
#include <string>

namespace cleat {

    struct Packet {

        // 响应状态(与 Resp 语义一致, 收敛于此)
        enum class Status { Success, Accepted, Error };

        static const char* StatusName(Status s) {
            switch (s) {
            case Status::Success:  return "success";
            case Status::Accepted: return "accepted";
            case Status::Error:    return "error";
            }
            return "error";
        }

        // 成功响应: {"status":"success","data":{...}}
        static nlohmann::json Ok(nlohmann::json data = nlohmann::json::object()) {
            return { {"status", "success"}, {"data", std::move(data)} };
        }

        // 接受响应(异步任务已启动): {"status":"accepted","data":{...}}
        static nlohmann::json Accepted(nlohmann::json data = nlohmann::json::object()) {
            return { {"status", "accepted"}, {"data", std::move(data)} };
        }

        // 错误响应: {"status":"error","message":"...","code":"..."} (code 可选)
        static nlohmann::json Error(const std::string& message, const std::string& code = "") {
            nlohmann::json j = { {"status", "error"}, {"message", message} };
            if (!code.empty()) {
                j["code"] = code;
            }
            return j;
        }

        // 通用构造(需要显式状态时用, 通常用 Ok/Accepted/Error 即可)
        static nlohmann::json Response(Status status, nlohmann::json data = nlohmann::json::object()) {
            return { {"status", StatusName(status)}, {"data", std::move(data)} };
        }

        // A→S 主动推送: {"cmd":..., "data":{...}}
        static nlohmann::json Push(const std::string& cmd, nlohmann::json data = nlohmann::json::object()) {
            return { {"cmd", cmd}, {"data", std::move(data)} };
        }
    };

} // namespace cleat
