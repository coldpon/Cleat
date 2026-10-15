/**
 * @file OutboundMessages.hpp
 * @brief 出站消息构造层(组包收口 M1): 消息语义 + SendOptions 声明**一体**
 * @details
 * **病灶**(收口前): 六类上行帧各有构造点(注册在 AgentContext、心跳在装配 lambda、
 * 事件在 EventReporter、响应/错误在 CommandProcessor 的 4 条路径), 各自手拼 JSON
 * 并各自声明 SendOptions —— "怎么组帧"与"怎么向限流器声明身份"分家, 载荷段
 * (params/data)与关联字段(session_id/request_id)的落点靠每个调用点自己记惯例。
 *
 * **本层的规则**: 每类消息一个具名构造函数, 返回 `{payload, opt}`(Outbound);
 * 信封(ts/session_id 顶层注入)**仍由 FrameBuilder 在出口注入** —— 不在构造时
 * 固化, 因为 session_id 是运行时状态(EventReport 队列可能在注册前入队)。
 * 出口统一走 `msg::Send`(Build + sink.Send 两步只有这一处)。
 *
 * **零改线约束**(与服务端契约锁步, 改动前须逐条核对 TS 侧读取点):
 *   - 心跳帧**必须**带 `params.session_id`: 服务端会话接管
 *     (`reconnectExisting` → `msgSessionID`)**只读 params** —— 顶层注入兜不了它;
 *   - 事件帧的 `data.ts`(毫秒)与信封顶层 `ts`(秒)撞名不同义, 但 data 段被
 *     整帧转发前端/落盘, 本批不改名(已登记为已知妥协);
 *   - 错误帧不带 origin_cmd(服务端把带 cmd 的上行帧当主动消息分发), 只有
 *     命令响应挂 origin_cmd —— kind/cmd 声明与载荷字段是两回事, 勿混。
 */
#pragma once

#include "Common/Protocol/CmdDef.hpp"
#include "Common/Protocol/FrameBuilder.hpp"
#include "Common/Protocol/Packet.hpp"
#include "Common/ThirdParty/json.hpp"
#include "Core/Identity/SessionIdentity.hpp"
#include "Core/Comms/Channel/SendResult.hpp"
#include "Core/Comms/OutboundSink.hpp"

namespace cleat {
    namespace msg {

        /// 组包产物: 载荷 + 出站声明(kind/cmd 限流身份, A.2.1)
        struct Outbound {
            nlohmann::json payload;
            /// 二进制响应体(混合帧 body): 非空 = 走 FrameBuilder 两参重载
            /// (JSON头 + '\0' + body, body_size 恒写)。截图等大载荷命令经
            /// CommandContext::binary_out 递入, 避免 base64 塞 JSON(+33% 体积)。
            std::vector<uint8_t> binary;
            SendOptions opt;
        };

        /// 出口: 信封注入(FrameBuilder)与投递(OutboundSink)的唯一两步。
        /// 之前每个调用点自己 Build+Send, "组好了却忘了 Build"这类错无从防范。
        /// binary 非空时走混合帧形态 —— 与流/文件分块上行同构(形状归一)。
        inline SendResult Send(OutboundSink& sink, Outbound o) {
            if (o.binary.empty()) {
                return sink.Send(FrameBuilder::Build(o.payload), o.opt);
            }
            return sink.Send(FrameBuilder::Build(o.payload, o.binary), o.opt);
        }

        // ------------------------------------------------------------------
        // 心跳(Push 通道保活; Poll 通道的 GET 本身即保活, 不产生本帧)
        // ------------------------------------------------------------------
        inline Outbound HeartbeatPing() {
            Outbound o;
            o.payload["cmd"] = Cmd::Heartbeat::Ping();
            // params.session_id 是服务端 msgSessionID 的**唯一**读取点(会话接管),
            // 不能依赖顶层注入 —— 见文件头"零改线约束"第一条。
            o.payload["params"]["session_id"] = SessionIdentity::Instance().GetSessionId();
            o.opt.kind = SendKind::Heartbeat;    // 无按命令限流身份(A.2.4 允许空 cmd)
            o.opt.cmd = o.payload["cmd"];
            return o;
        }

        // ------------------------------------------------------------------
        // 注册帧(首连/重连): 业务参数由调用方组装(身份来源在 AgentContext)
        // ------------------------------------------------------------------
        inline Outbound FirstConnect(nlohmann::json params) {
            Outbound o;
            o.payload["cmd"] = Cmd::FirstConnect();
            o.payload["params"] = std::move(params);
            o.opt.kind = SendKind::Response;     // 注册帧按命令身份限流(低带宽通道可拦)
            o.opt.cmd = o.payload["cmd"];
            return o;
        }

        // ------------------------------------------------------------------
        // 事件上报(遥测): {"cmd":"EventReport","data":{...}}
        // data.session_id 在**发送时**由 Drain 重绑(帧可能注册前入队), 构造时
        // 写入的值只是当时快照 —— 见 EventReporter::Drain。
        // ------------------------------------------------------------------
        inline Outbound EventReport(nlohmann::json data) {
            Outbound o;
            o.payload = Packet::Push(Cmd::Telemetry::EventReport(), std::move(data));
            o.opt.kind = SendKind::Event;        // 遥测: 无按命令限流身份
            return o;
        }

        // ------------------------------------------------------------------
        // 命令响应(成功/业务结果): 挂 origin_cmd + session_id + request_id。
        // session_id 在**本层显式挂载**(sid!=0 才写)而不依赖 FrameBuilder 的
        // provider —— "响应帧必带会话身份"是无条件契约, 不应取决于装配是否接线;
        // FrameBuilder 的顶层注入对已有键不覆盖, 两处同值不冲突。
        // ------------------------------------------------------------------
        inline Outbound Response(const std::string& cmd, const std::string& request_id,
            nlohmann::json result) {
            Outbound o;
            o.payload = std::move(result);
            // 不能用 "cmd": 服务端把带 cmd 的上行帧一律当主动消息分发(实测),
            // origin_cmd 是响应专用标记, 服务端按未知字段忽略。
            o.payload["origin_cmd"] = cmd;
            const uint64_t sid = SessionIdentity::Instance().GetSessionId();
            if (sid != 0) {
                o.payload["session_id"] = sid;
            }
            if (!request_id.empty()) {
                o.payload["request_id"] = request_id;
            }
            o.opt.kind = SendKind::Response;
            o.opt.cmd = cmd;
            return o;
        }

        /// 带二进制体的响应(混合帧): data 段只放元数据(format/quality 等),
        /// 大载荷走 body —— 服务端 decodeResponse 拆出后提升为 data.image。
        inline Outbound Response(const std::string& cmd, const std::string& request_id,
            nlohmann::json result, std::vector<uint8_t> binary) {
            Outbound o = Response(cmd, request_id, std::move(result));
            o.binary = std::move(binary);
            return o;
        }

        // ------------------------------------------------------------------
        // 错误响应: {"status":"error",...} + session_id + request_id; **不带**
        // origin_cmd(解析失败帧无从归属命令; 超时/占满类错误帧历史落线即不带,
        // 保持一致)。cmd 非空时声明为限流身份(如超时回执), 空 = 如实不参与
        // 按命令限流。
        // ------------------------------------------------------------------
        inline Outbound Error(const std::string& message, const std::string& code,
            const std::string& request_id, const std::string& cmd = {}) {
            Outbound o;
            o.payload = Packet::Error(message, code);
            const uint64_t sid = SessionIdentity::Instance().GetSessionId();
            if (sid != 0) {
                o.payload["session_id"] = sid;
            }
            if (!request_id.empty()) {
                o.payload["request_id"] = request_id;
            }
            o.opt.kind = SendKind::Response;
            o.opt.cmd = cmd;
            return o;
        }

    } // namespace msg
} // namespace cleat
