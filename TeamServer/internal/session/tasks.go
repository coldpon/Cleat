// 任务编排: 操作员命令的异步任务生命周期(pending/dispatched/completed/failed/timeout/superseded)。
// 复用现有 pull 通道 + request_id 匹配(PendingResp): 任务先落 store, 再入会话 pull 队列;
// Agent 离线时任务停留在 pending, 上线自动补偿; 响应按 request_id 精确回写 store。
//
// 第 3 批状态机收口:
package session

import (
	"encoding/json"
	"fmt"
	"time"

	"cleat/teamserver/internal/logx"
	"cleat/teamserver/internal/store"
)

// 护栏只拦异常 —— 过大帧会把 beacon 节拍响应体撑爆(GET 响应承载 outbox 帧)。
const MaxTaskPayloadBytes = 32 << 20

// SubmitTask 持久化一条命令任务并(若会话在线)立即派发。
// 返回任务 ID; 会话在线时后台等待响应并回写 store。
func (m *Manager) SubmitTask(sid int, cmd string, params map[string]interface{}, mode string) (int64, error) {
	paramsJSON := ""
	if params != nil {
		b, err := json.Marshal(params)
		if err != nil {
			return 0, err
		}
		paramsJSON = string(b)
	}
	task := &store.Task{
		SessionID: sid,
		Cmd:       cmd,
		Params:    paramsJSON,
		RequestID: fmt.Sprintf("req-%d-%d", sid, time.Now().UnixNano()),
		Mode:      mode,
		Status:    store.TaskPending,
	}
	id, err := m.st.SaveTask(task)
	if err != nil {
		return 0, err
	}
	// 场景: 旧任务卡 pending(Agent 离线)或卡等待, 操作员等不及重发同一命令 ——
	// 若不取代, Agent 会依次执行两份。必须在新任务派发(compensate)之前完成让位。
	m.supersedeDuplicates(sid, cmd, paramsJSON, id)
	if s := m.sessionByID(sid); s != nil {
		// 会话在线: 先把该会话遗留的未完成任务一并补派发(补偿), 再派发本条。
		m.compensate(s)
	}
	return id, nil
}

// supersedeDuplicates 把同会话同命令同参数的未终态任务(pending/dispatched)置为
// superseded 终态, 并撤回未取走帧 + 取消其等待 goroutine。
//
// 防重蹈覆辙: **先取消等待者再写状态**的顺序不可换 —— 若先写状态后取消, 等待 goroutine
// 的响应分支/超时分支可能并发回写 completed/failed/timeout, 覆盖 superseded(状态机撕裂)。
// cancelTaskWait 原子地(同锁内)关闭取消信号 + 摘除 PendingResp, 保证:
//   - 等待 goroutine 从 cancel 分支退出, 不再碰任务状态;
//   - 晚到响应走复活分支时状态是 superseded ≠ timeout → 不复活, 作废(操作员已重发, 旧响应无意义)。
//
// 固有限制(记录不修): 帧已交付 Agent 的旧任务无法阻止其被执行(下行已不可撤),
// superseded 只保证簿记干净 + 撤回未取走帧。
func (m *Manager) supersedeDuplicates(sid int, cmd, paramsJSON string, newID int64) {
	for _, t := range m.st.TasksBySession(sid) {
		if t.ID == newID || t.Cmd != cmd || t.Params != paramsJSON {
			continue
		}
		if t.Status != store.TaskPending && t.Status != store.TaskDispatched {
			continue // 终态(completed/failed/timeout/superseded)是历史记录, 不动
		}
		if s := m.sessionByID(sid); s != nil {
			// 撤回未取走帧(仅 dispatched 有帧; 已交付则撤回失败, 属固有限制)
			s.RemoveOutboxByRequestID(t.RequestID)
			// 取消等待 goroutine(先于状态写入, 见函数头防重蹈覆辙注)
			s.cancelTaskWait(t.RequestID)
		}
		// CAS 写入(状态机铁律): supersede 与响应回写/超时回写并发时, 谁先到谁赢 ——
		// 若响应恰在取消生效前已把任务写成 completed/failed(终态), 两个 CAS 都失败,
		// 保留真实终态, 不被 superseded 覆盖。
		if !m.st.UpdateTaskStatusIf(t.ID, store.TaskPending, store.TaskSuperseded,
			fmt.Sprintf("superseded by task #%d (duplicate command resubmitted)", newID)) {
			m.st.UpdateTaskStatusIf(t.ID, store.TaskDispatched, store.TaskSuperseded,
				fmt.Sprintf("superseded by task #%d (duplicate command resubmitted)", newID))
		}
		logx.Printf("[tasks] 任务 %d(%s) 被新提交 #%d 取代\n", t.ID, t.Cmd, newID)
	}
}

// compensate 把会话的未完成任务(pending)重新派发到 pull 队列并挂响应回收。
func (m *Manager) compensate(s *Session) {
	// **派发互斥**: 并发 compensate(多路 GET/handlePoll/SubmitTask)会各拿
	// 同一 PendingTasks 快照 → 同任务 dispatchTask 两次(outbox 双份 + PendingResp 覆盖
	// + 60s 后双双超时回滚 → 再派发, 循环放大)。整循环持 dispatchMu: 后一轮进入时
	// 前一轮已把任务置 Dispatched, 快照不再命中 pending。
	// 已收紧, 调用方兜过滤是隐性依赖, 见 store/tasks.go 防重蹈覆辙注。)
	s.dispatchMu.Lock()
	defer s.dispatchMu.Unlock()
	for _, t := range m.st.PendingTasks(s.ID()) {
		m.dispatchTask(s, &t)
	}
}

// (如 ModuleLoad 的 DLL 字节), 无载荷返回 (nil, nil)。启动期注入(main.go), 运行期只读。
// session 包不知道 modules 包 —— 分层: 解析器闭包由上层(service)组装。
// 不得在调用方手搓"SaveTask→手动置 dispatched→goroutine 同步等待"的平行路径 ——
// 那条旧路径曾同时造成: 状态撒谎(帧未发即 dispatched)/崩溃恢复重派缺字节/cmdMu 占 60s。
func (m *Manager) SetPayloadResolver(fn func(sid int, cmd string, params map[string]interface{}) ([]byte, error)) {
	m.payloadResolver = fn
}

// dispatchTask 把一条任务派发到会话, 并后台等待响应回写 store。
// 下行统一一条路径(节拍合一 S3): sendFrame —— TCP 写连接(push 直发),
// beacon 入 outbox 由 Agent GET/上传请求响应体取走。大载荷命令组 hybrid 帧
// (JSON头 + '\0' + 二进制体, 与 SendHybrid 帧格式逐字节一致, Agent 无感)。
// 原"TCP 入 CmdQueue 待 poll 弹出"分支已删: CmdQueue 的唯一消费者是 beacon 的
// handlePoll, 而 TCP 侧从不发 poll 帧 —— 该分支是只进不出的死路, 随队列合一消亡。
func (m *Manager) dispatchTask(s *Session, t *store.Task) {
	var params map[string]interface{}
	if t.Params != "" {
		_ = json.Unmarshal([]byte(t.Params), &params)
	}

	// 与 sendFrame 失败留 pending 等补偿的语义不同)。
	var bin []byte
	if m.payloadResolver != nil {
		b, err := m.payloadResolver(s.ID(), t.Cmd, params)
		if err != nil {
			m.st.UpdateTaskStatus(t.ID, store.TaskFailed, "payload unavailable: "+err.Error())
			return
		}
		if len(b) > MaxTaskPayloadBytes {
			m.st.UpdateTaskStatus(t.ID, store.TaskFailed,
				fmt.Sprintf("payload %dB exceeds limit %dB", len(b), MaxTaskPayloadBytes))
			return
		}
		bin = b
	}

	req := map[string]interface{}{
		"cmd":        t.Cmd,
		"params":     params,
		"request_id": t.RequestID,
		"ts":         time.Now().Unix(),
	}
	if bin != nil {
		req["body_size"] = len(bin) // 与 SendHybrid 对齐
	}
	head, err := json.Marshal(req)
	if err != nil {
		return
	}
	body := head
	if bin != nil {
		body = make([]byte, 0, len(head)+1+len(bin))
		body = append(body, head...)
		body = append(body, 0)
		body = append(body, bin...)
	}
	if err := s.sendFrame(body); err != nil {
		// 静默 return: 任务留 pending —— 会话此刻死亡, Agent 重连后 compensate 重派。
		return
	}

	// **先注册等待者再置 dispatched**: beacon 下响应早到不丢。
	respCh := make(chan []byte, 1)
	cancelCh := make(chan struct{})
	s.pendingMu.Lock()
	s.PendingResp[t.RequestID] = respCh
	s.taskWaits[t.RequestID] = cancelCh
	s.pendingMu.Unlock()

	// CAS 派发(状态机铁律): pending→dispatched 只能从这里走。CAS 失败 = 任务已被
	// supersede/并发派发抢先迁移 —— 丢弃本次派发(含刚注册的等待者, 由 cleanup 摘除)。
	// 防的场景: 另一 goroutine 的 compensate 快照与本 goroutine 的 supersede 竞态,
	// 快照里的任务在派发前一刻已被置 superseded → 无 CAS 则会派发一个已死任务。
	if !m.st.UpdateTaskStatusIf(t.ID, store.TaskPending, store.TaskDispatched, "") {
		cleanupPending(s, t.RequestID)
		return
	}

	// 后台等响应: 成功/失败回写; 超时按帧是否被取走分流(防双执行, 下述)。
	// **回滚语义**: 等待窗 = 3×RespTimeout —— beacon 空闲节拍
	// 默认 60s 与 RespTimeout 相等, 帧**还在 outbox 未被取走**就回滚, 下次 compensate
	// 会再派一份 → Agent 依次执行多份同一命令(重复执行事故)。3 倍窗给 Agent 至少
	// 两个完整节拍取件; 到点后按帧是否仍在 outbox 分流(见下)。
	// 闭包内读全局 var 会与并发改写竞争(race 实测) —— 启动前求值一次, 闭包只读局部。
	waitWindow := 3 * RespTimeout
	go func(taskID int64, reqID string) {
		defer logx.Guard("task-wait") // G3
		defer cleanupPending(s, reqID)
		select {
		case respData, ok := <-respCh:
			if !ok {
				return // ch 永不 close, 此分支防御性保留
			}
			// CAS(状态机铁律): dispatched→终态。CAS 失败 = 等待期间任务已被取代
			// (cancelTaskWait 尚未生效的窗口)或已被保险丝扫掉 —— 响应作废, 不覆盖。
			status, result := statusFromResponse(respData)
			if !m.st.UpdateTaskStatusIf(taskID, store.TaskDispatched, status, result) {
				logx.Printf("[tasks] 任务 %d 响应到达但状态已迁移(被取代/已扫), 作废\n", taskID)
			}
		case <-cancelCh:
			return
		case <-time.After(waitWindow):
			// **以 Remove 返回值为准**: 上一版先
			// Contains 预判再 Remove, 两步之间并发 GET 可取走帧, Remove=false 被无视,
			// 任务回滚 pending 后下轮 compensate 重派 → Agent 双执行。
			// Remove=true = 帧还在 outbox(未取件) → 撤回成功, 回滚 pending;
			if s.RemoveOutboxByRequestID(reqID) {
				m.st.UpdateTaskStatusIf(taskID, store.TaskDispatched,
					store.TaskPending, "timed out in outbox; frame withdrawn")
				return
			}
			// (TaskTimeout 零引用, 全仓无人再碰它)。现接终态: timeout = 命运未知
			if m.st.UpdateTaskStatusIf(taskID, store.TaskDispatched, store.TaskTimeout,
				"delivered but no response within 3x window; not re-dispatched") {
				logx.Printf("[tasks] 任务 %d 已交付超 3x 窗未响应, 置 timeout(晚到响应可复活)\n", taskID)
			}
		} // select 闭合
	}(t.ID, t.RequestID)
}

// statusFromResponse 把 Agent 响应体映射为任务终态(completed/failed)与结果串。
func statusFromResponse(respData []byte) (store.TaskStatus, string) {
	var resp map[string]interface{}
	status := store.TaskFailed
	if err := json.Unmarshal(respData, &resp); err == nil {
		if resp["status"] == "success" || resp["status"] == "ok" {
			status = store.TaskCompleted
		}
	}
	return status, string(respData)
}

// cleanupPending 等待者结束(正常/取消)后从会话两张注册表摘除条目, 防陈旧 request_id 累积。
func cleanupPending(s *Session, reqID string) {
	s.pendingMu.Lock()
	delete(s.PendingResp, reqID)
	delete(s.taskWaits, reqID)
	s.pendingMu.Unlock()
}

// PendingResp(响应不再投给已死的等待者, 晚到响应改走 reviveLateResponse 分支)。
// 注册表无此 rid(如会话离线后 MarkOffline 已清空表)时静默返回 —— 状态正确性由
// 等待 goroutine 侧的 CAS 兜底(它写终态前发现状态已非 dispatched 即放弃), 本方法
// 只负责让取消**及时**生效, 不承载正确性。
func (s *Session) cancelTaskWait(reqID string) {
	s.pendingMu.Lock()
	ch, ok := s.taskWaits[reqID]
	delete(s.taskWaits, reqID)
	delete(s.PendingResp, reqID)
	s.pendingMu.Unlock()
	if ok && ch != nil {
		close(ch) // 只 close 这个专用于取消的信号 channel; PendingResp 的 respCh 永不 close(见 MarkOffline 重构警告)
	}
}

// (等待 goroutine 3x 窗退出), 查 store —— 若任务状态恰为 timeout, 按响应内容单向复活为
// completed/failed。价值: 慢通道(HTTPS/DNS 长节拍)下 3x 窗不够是常态而非异常, 复活让
// 前端显示事实而不是误报。
//
// **只允许 timeout → 终态, 单向**: dispatched 的等待 goroutine 还活着, 响应侧再写即双写竞态;
// pending→终态则绕过派发语义。superseded 的晚到响应作废(操作员已重发, 旧响应无意义)。
// 已知限制(记录不修): 等待 goroutine 走超时分支与 cleanupPending 之间响应投进缓冲 ch
// 无人读 → 该次响应丢失, 任务留 timeout。窗口 <1ms, 接受。
func (m *Manager) reviveLateResponse(rid string, respData []byte) {
	t, ok := m.st.TaskByRequestID(rid)
	if !ok || t.Status != store.TaskTimeout {
		return
	}
	status, result := statusFromResponse(respData)
	// CAS(状态机铁律): timeout→终态恰好一个响应赢家 —— 两个晚到响应并发复活同一
	// 任务时只有一个成功(先到先得), 后到作废, 无双写。
	if m.st.UpdateTaskStatusIf(t.ID, store.TaskTimeout, status, result) {
		logx.Printf("[tasks] 任务 %d 晚到响应复活: timeout → %s\n", t.ID, status)
	}
}

// 10×RespTimeout → timeout。正常路径任务活不过 3x 窗(等待 goroutine 兜着);
// 能到 10x 的都是 goroutine 已死(Guard 捕 panic 后退出)/store 被外部改过的异常态。
// 定位是**保险丝**非主路径, 参数取宽防误杀。返回本轮标记数(诊断用)。
// 由清理 sweeper 周期调用(与 SweepStaleBeacons/CleanupExpired 同节奏, main.go 挂载)。
func (m *Manager) SweepStuckTasks() int {
	cutoff := (10 * RespTimeout).Milliseconds()
	now := time.Now().UnixMilli()
	n := 0
	for _, t := range m.st.AllTasks() {
		if t.Status == store.TaskDispatched && now-t.UpdatedAt > cutoff {
			// CAS(状态机铁律): 只扫真正还卡在 dispatched 的; 期间被响应/取代抢先迁移的跳过。
			if m.st.UpdateTaskStatusIf(t.ID, store.TaskDispatched, store.TaskTimeout,
				"stuck dispatched swept (waiter dead)") {
				logx.Printf("[tasks] 任务 %d 卡 dispatched 超保险丝阈值, 置 timeout\n", t.ID)
				n++
			}
		}
	}
	return n
}
