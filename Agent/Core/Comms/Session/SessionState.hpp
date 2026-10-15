/**
 * @file SessionState.hpp
 * @brief 会话状态与事件
 * @details
 * **这一层此前不存在**: 会话状态原先摊在 `ConnectionManager` 的 17 个标志位上
 * (`awaiting_confirm_` / `session_confirm_ok_` / `session_denied_` /
 * `first_connect_done_` / 两个循环各自的 retry_delay ...), 5 个 bool 可组成 32 种
 * 组合而多数非法 —— "假成功"与状态错配正是从这里长出来的。
 *
 * 本文件把状态收敛为**一个枚举 + 一张转移表**, 非法组合在类型上不存在。
 *
 * ```
 * Idle ──OpenRequested──▶ Opening ──ChannelUp{无需握手}──▶ Registering
 *                            │      └─ChannelUp{需握手}──▶ Handshaking
 *                            │                                  │
 *                            │                          HandshakeSent
 *                            ▼                                  ▼
 *                        Reconnecting ◀─────────────────── Registering
 *                            │                                  │
 *                     BackoffElapsed                        Confirm(sid)
 *                            │                                  ▼
 *                            └───────────────────────────▶  Ready
 *   (Handshaking/Registering/Ready) ──ChannelDown/Denied/ChannelsReconfigured──▶ Reconnecting
 *   任意状态 ──Stop──▶ Stopping
 * ```
 *
 * **代际(epoch, )**: 每次进入 `Registering`(即每次 FirstConnect 尝试)递增。
 * 它是"可见产物"的归属标记 —— 迟到响应与陈旧确认帧据此被识别并丢弃;
 * 注意它**不隔离副作用**。
 */
#pragma once

#include <cstdint>

namespace cleat {

    /// 会话状态(显式)
    enum class SessionState {
        Idle = 0,          ///< 未建立通道
        Opening = 1,       ///< 正在打开通道(Chain 连接中)
        Handshaking = 2,   ///< 通道就绪, 握手帧已发出/待发
        Registering = 3,   ///< FirstConnect 已发, 等待确认(代际在此递增)
        Ready = 4,         ///< 已注册(session_id 已拿到)
        Reconnecting = 5,  ///< 链路断开/被拒/换链, 待重连(退避计时由 Backoff 持有)
        Stopping = 6,      ///< 正在停止
        Count_ = 7
    };

    inline const char* ToString(SessionState s) {
        switch (s) {
        case SessionState::Idle: return "Idle";
        case SessionState::Opening: return "Opening";
        case SessionState::Handshaking: return "Handshaking";
        case SessionState::Registering: return "Registering";
        case SessionState::Ready: return "Ready";
        case SessionState::Reconnecting: return "Reconnecting";
        case SessionState::Stopping: return "Stopping";
        default: return "?";
        }
    }

    /// 会话事件(状态机只接受这几种; 其余一概拒绝并计数)
    enum class SessionEventKind {
        OpenRequested = 0,        ///< 请求建链(启动 / 退避到期 / 换链后)
        ChannelUp = 1,            ///< 通道就绪(Chain::Connect 成功)
        OpenFailed = 2,           ///< 通道打开失败
        HandshakeSent = 3,        ///< 握手帧已发出(PSK 模式无此步, 直接 Registering)
        Confirm = 4,              ///< 收到 FirstConnect 确认(携带 session_id 与接收时代际)
        Denied = 5,               ///< 服务端明确拒绝(如 DENY_BUSY)
        ChannelDown = 6,          ///< 链路断开(通道自报故障)
        BackoffElapsed = 7,       ///< 退避计时到期, 该重试了
        ChannelsReconfigured = 8, ///< SetConfig 下发新通道链(旧链已失效)
        Stop = 9,                 ///< 停止
        /// 服务端不认得本机 session_id(SESSION_UNKNOWN, 携带它不认得的那个 sid)。
        /// 与 Denied 的根本区别: Denied 说"你的会话还在, 但这次连接不许占用"(身份仍
        /// 有效); SessionLost 说"我这边根本没有这个会话"(身份已失效) → 必须**清空
        /// 本机 session_id** 再重注册, 否则后续每帧都带着一个服务端不认的 sid 空转。
        SessionLost = 10
    };

    inline const char* ToString(SessionEventKind k) {
        switch (k) {
        case SessionEventKind::OpenRequested: return "OpenRequested";
        case SessionEventKind::ChannelUp: return "ChannelUp";
        case SessionEventKind::OpenFailed: return "OpenFailed";
        case SessionEventKind::HandshakeSent: return "HandshakeSent";
        case SessionEventKind::Confirm: return "Confirm";
        case SessionEventKind::Denied: return "Denied";
        case SessionEventKind::ChannelDown: return "ChannelDown";
        case SessionEventKind::BackoffElapsed: return "BackoffElapsed";
        case SessionEventKind::ChannelsReconfigured: return "ChannelsReconfigured";
        case SessionEventKind::Stop: return "Stop";
        case SessionEventKind::SessionLost: return "SessionLost";
        default: return "?";
        }
    }

    /**
     * @brief 会话事件
     * @details 用平铺字段而非 variant: 事件种类少、字段语义窄, 平铺更易读且无分配。
     */
    struct SessionEvent {
        SessionEventKind kind = SessionEventKind::OpenRequested;
        /// ChannelUp 时有效: 是否需要握手步骤(PSK 模式为 false → 直接 Registering)
        bool handshake_required = false;
        /// Confirm 时有效: 服务端分配的会话 id(0 = 非法, 拒绝)
        uint64_t session_id = 0;
        /// Confirm 时有效: 该帧**接收时**的连接代际。与本机当前代际不符即为陈旧帧 → 丢弃
        uint64_t epoch = 0;
        /// 日志用原因(可为空)
        const char* why = nullptr;

        static SessionEvent Make(SessionEventKind k, const char* why = nullptr) {
            SessionEvent e;
            e.kind = k;
            e.why = why;
            return e;
        }
    };

    /**
     * @brief 合法转移表(唯一来源)
     * @param from 当前状态
     * @param ev   事件(需读其载荷: ChannelUp 携带 `handshake_required`)
     * @return 目标状态; `-1` 表示该 (state, event) 组合非法(调用方应拒绝并计数)
     * @details 表即契约: 新增事件时必须在此登记, 否则一律被视为非法转移。
     */
    inline int NextState(SessionState from, const SessionEvent& ev) {
        // Stop 可从任意状态进入 Stopping(幂等: 已在 Stopping 也允许)
        if (ev.kind == SessionEventKind::Stop) return static_cast<int>(SessionState::Stopping);

        switch (from) {
        case SessionState::Idle:
            switch (ev.kind) {
            case SessionEventKind::OpenRequested: return static_cast<int>(SessionState::Opening);
            default: return -1;
            }
        case SessionState::Opening:
            switch (ev.kind) {
            // 通道就绪: 是否需要握手由事件携带 —— X25519 模式走 Handshaking,
            // PSK 模式无握手帧(BuildHandshakeFrame 返回空), 直接进 Registering。
            case SessionEventKind::ChannelUp:
                return static_cast<int>(ev.handshake_required ? SessionState::Handshaking
                                                             : SessionState::Registering);
            case SessionEventKind::OpenFailed: return static_cast<int>(SessionState::Reconnecting);
            default: return -1;
            }
        case SessionState::Handshaking:
            switch (ev.kind) {
            case SessionEventKind::HandshakeSent: return static_cast<int>(SessionState::Registering);
            case SessionEventKind::ChannelDown: return static_cast<int>(SessionState::Reconnecting);
            default: return -1;
            }
        case SessionState::Registering:
            switch (ev.kind) {
            case SessionEventKind::Confirm: return static_cast<int>(SessionState::Ready);
            case SessionEventKind::Denied: return static_cast<int>(SessionState::Reconnecting);
            case SessionEventKind::ChannelDown: return static_cast<int>(SessionState::Reconnecting);
            case SessionEventKind::ChannelsReconfigured: return static_cast<int>(SessionState::Reconnecting);
            // 会话被服务端遗忘: 清身份后与"被拒"同路(重连 → 重新注册)
            case SessionEventKind::SessionLost: return static_cast<int>(SessionState::Reconnecting);
            default: return -1;
            }
        case SessionState::Ready:
            switch (ev.kind) {
            case SessionEventKind::ChannelDown: return static_cast<int>(SessionState::Reconnecting);
            case SessionEventKind::Denied: return static_cast<int>(SessionState::Reconnecting);
            case SessionEventKind::ChannelsReconfigured: return static_cast<int>(SessionState::Reconnecting);
            // **本事件的主战场**: Ready 态收到 SESSION_UNKNOWN = 服务端不认得本机 sid
            // (服务端重启后持久记录也没了 / 记录被 cleanup 清掉)。
            case SessionEventKind::SessionLost: return static_cast<int>(SessionState::Reconnecting);
            default: return -1;
            }
        case SessionState::Reconnecting:
            switch (ev.kind) {
            case SessionEventKind::BackoffElapsed: return static_cast<int>(SessionState::Opening);
            // 换链事件在重连态重复到达是合法的(幂等: 仍停在 Reconnecting)
            case SessionEventKind::ChannelsReconfigured: return static_cast<int>(SessionState::Reconnecting);
            default: return -1;
            }
        case SessionState::Stopping:
            // 停止后只接受 Stop(幂等); 其余一律非法
            return -1;
        default:
            return -1;
        }
    }

    inline bool IsTransitionLegal(SessionState from, const SessionEvent& ev) {
        return NextState(from, ev) >= 0;
    }

} // namespace cleat
