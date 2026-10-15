package session

import (
	"encoding/json"
	"time"
)

// handlePoll 处理 pull 通道 poll 帧(节拍合一 S3 后的**兼容形态**)。
//
// 历史职责(已移除): 从 CmdQueue 弹出命令、包一层 data.command 响应 —— 下行队列合一后
// CmdQueue 消亡, 下行统一 outbox(经 GET 身份块取件 / 兼容期经 POST 响应体取件),
// Agent 侧 FrameKind::PollResponse 与 data.command 特例同步删除(A 批 A4)。
//
// 保留职责: Touch(心跳) + compensate(离线任务补偿 —— 派发动作把任务帧直写 outbox/连接,
// 不经本响应)。响应体恒为"空命令"的成功帧, 让旧 Agent 的 poll request_id 匹配不悬空;
// Agent 升级后(不再发 poll 帧)此 handler 自然失去调用方, 保留无害。
func (m *Manager) handlePoll(s *Session, msg map[string]interface{}) {
	s.Touch()
	m.compensate(s)
	resp := map[string]interface{}{
		"status": "success",
		"data": map[string]interface{}{
			"command": nil,
		},
		"ts": time.Now().Unix(),
	}
	if rid, ok := msg["request_id"].(string); ok && rid != "" {
		resp["request_id"] = rid
	}
	body, err := json.Marshal(resp)
	if err != nil {
		return
	}
	_ = s.sendFrame(body)
}
