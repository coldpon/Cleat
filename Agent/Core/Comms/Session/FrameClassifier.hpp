/**
 * @file FrameClassifier.hpp
 * @brief 帧分类(纯函数, 无状态无 I/O)
 * @details
 * **为什么单独存在**: 分类回答"这帧是什么", 路由回答
 * "该拿它做什么"。会话层必须**消费分类结果**才能产生事件(确认 / DENY_BUSY) ——
 * 故分类是会话层的**前置输入**, 与状态推进解耦: 判定规则全部收敛为单一纯函数。
 *
 * **判定次序(硬约束, 不可调换)**:
 *   ① DENY_BUSY 拒绝 → ② Poll 响应 → ③ FirstConnect 确认 → ④ 普通命令
 * 原因: Poll 响应与确认帧**形状重叠**(都是 `status=success` + `data`), 只能靠
 * `data.command` 与 `data.session_id` 区分; 若先判确认, Poll 捎带的命令会被丢弃。
 *
 * 纯函数性质: 相同输入必得相同输出, 不读全局、不改状态 → 可直接单测,
 * 也可被会话状态机与路由层共享。
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Common/ThirdParty/json.hpp"

namespace cleat {

    /// 帧类别
    enum class FrameKind {
        Denied = 0,        ///< 服务端明确拒绝(当前仅 DENY_BUSY)→ 会话层立即按失败处理
        SessionConfirm = 1,///< FirstConnect 确认(携带 session_id)
        Command = 3,       ///< 普通命令帧(cmd 字段)
        Ignored = 4,       ///< 已解析但无动作(非 DENY_BUSY 的错误帧等)
        Unknown = 5,       ///< 无法解析(调用方按既有约定回错误帧)
        /// 服务端不认得本机 session_id(SESSION_UNKNOWN)→ **必须清身份重新注册**
        /// @details 与 Denied 的区别是承重的: DENY_BUSY 说的是"你的会话还在, 但这次连接
        ///          不许占用"(身份仍有效, 退避后重试即可); SESSION_UNKNOWN 说的是
        ///          "我这边根本没有这个会话"(身份已失效, 带着旧 sid 继续发只是空转)。
        ///          且它**恰好在 `Ready` 态到达** —— 而 Denied 的处理被限定在注册窗口内
        ///          (见 SessionRunner::Tick), 故两者不可合并。
        SessionUnknown = 6
    };

    inline const char* ToString(FrameKind k) {
        switch (k) {
        case FrameKind::Denied: return "Denied";
        case FrameKind::SessionConfirm: return "SessionConfirm";
        case FrameKind::Command: return "Command";
        case FrameKind::Ignored: return "Ignored";
        case FrameKind::Unknown: return "Unknown";
        case FrameKind::SessionUnknown: return "SessionUnknown";
        default: return "?";
        }
    }

    /// 分类结果(类型化; 各类只填自己用得到的字段)
    struct ClassifiedFrame {
        FrameKind kind = FrameKind::Unknown;

        // ---- Denied / SessionUnknown ----
        std::string code;                  ///< 服务端错误码(如 DENY_BUSY / SESSION_UNKNOWN)

        // ---- Command / PollResponse(Poll 捎带的命令) ----
        std::string cmd;
        std::string request_id;
        nlohmann::json params;             ///< 命令参数(对象)

        // ---- SessionConfirm / PollResponse / SessionUnknown ----
        /// PollResponse/Confirm: 服务端分配的会话 id;
        /// SessionUnknown: 服务端**不认得**的那个 sid(0 = 帧内本无 sid, 服务端无法路由)
        uint64_t session_id = 0;

        // ---- 混合帧(JSON头 + '\0' + 二进制体) ----
        std::vector<uint8_t> binary;

        // ---- 解析失败 ----
        std::string parse_error;           ///< 非空 = 解析异常, 调用方按既有约定回错误帧
    };

    /**
     * @brief 分类一帧(纯函数)
     * @param payload 完整明文帧(可能是混合帧)
     * @details 与重构前 `CommandProcessor::ProcessCommandPacket` 的判别次序**逐条一致**,
     *          差异仅在于结果被类型化而非直接产生副作用。
     */
    ClassifiedFrame ClassifyFrame(const std::vector<uint8_t>& payload);

} // namespace cleat
