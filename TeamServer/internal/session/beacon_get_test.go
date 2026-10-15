package session

import (
	"encoding/json"
	"fmt"
	"strings"
	"testing"
	"time"

	"cleat/teamserver/internal/eventbus"
	"cleat/teamserver/internal/store"
)

// registerBeacon 用 FirstConnect 注册一个 beacon 会话, 返回 (会话, sid)。
// 注册 confirm 在此取走丢弃, 隔离后续断言(需要 confirm 的用例自行再触发)。
func registerBeacon(t *testing.T, m *Manager, machineID string) (*Session, int) {
	t.Helper()
	sess := m.NewBeaconSession("10.0.0.9:4444")
	body, _ := json.Marshal(map[string]interface{}{
		"cmd": CmdFirstConnect,
		"ts":  float64(time.Now().Unix()),
		"params": map[string]interface{}{
			"machine_id": machineID, "hostname": "beacon-get-host",
		},
	})
	next, err := m.HandleBeaconFrame(sess, body)
	if err != nil {
		t.Fatalf("注册: %v", err)
	}
	sid := next.ID()
	if sid == 0 {
		t.Fatal("注册后 sid 不应为 0")
	}
	if f, ok := next.PollOutbox(); !ok || !strings.Contains(string(f), "session_id") {
		t.Fatalf("注册 confirm 应入 outbox, got %q ok=%v", f, ok)
	}
	return next, sid
}

// confirmSID 从 confirm 帧里解出 session_id。
func confirmSID(t *testing.T, frame []byte) int {
	t.Helper()
	var resp struct {
		Data struct {
			SessionID int `json:"session_id"`
		} `json:"data"`
	}
	if err := json.Unmarshal(frame, &resp); err != nil {
		t.Fatalf("confirm 帧解析: %v", err)
	}
	return resp.Data.SessionID
}

// TestBeaconGetMidClaimRestart S2.6 核心场景: 服务端重启(内存表空)后, Agent 以 mid 发 GET,
// 按持久记录复活原会话并经 confirm 帧拿回 **原 sid** —— 不新建、记录数不增。
// (复活分支: outbox 已随进程丢失, 认领**必须补发 confirm** —— 这是补发的唯一合法场景。)
func TestBeaconGetMidClaimRestart(t *testing.T) {
	st := store.New()
	m1 := NewManager(st, eventbus.New(), nil, nil, false)
	_, sid := registerBeacon(t, m1, "restart-m1")

	// 模拟服务端重启: 新 Manager 共享同一 store, 内存表为空
	m2 := NewManager(st, eventbus.New(), nil, nil, false)
	if got := len(m2.SessionInfos()); got != 0 {
		t.Fatalf("重启后内存表应为空, got %d", got)
	}

	frame := m2.BeaconGet("10.0.0.9:4444", "restart-m1", 0)
	if frame == nil {
		t.Fatal("mid 认领应返回 confirm 帧")
	}
	if got := confirmSID(t, frame); got != sid {
		t.Fatalf("confirm 应回带原 sid=%d, got %d", sid, got)
	}
	// 认领不得新建: store 中该机器记录仍是同一条(同 ID)
	if rec := m2.st.ByMachine("restart-m1"); rec == nil || rec.ID != sid {
		t.Fatalf("认领后应为原记录 ID=%d, got %+v", sid, rec)
	}
}

// TestBeaconGetMidClaimLive 内存中活跃 beacon 会话的 mid 认领( 新契约:
// 认领**不补 confirm** —— POST 注册时的 confirm 已在 outbox 队首, 无条件补发会造出
// 重复 confirm, Agent Ready 态丢弃并刷 WARN)。本用例锁定真实 interact 序列:
//
//	GET(mid) → 取到注册 confirm(队首) → GET(sid) → 取到旧命令 → GET → 空。
func TestBeaconGetMidClaimLive(t *testing.T) {
	m := newTestManager()
	sess := m.NewBeaconSession("10.0.0.9:4444")
	body, _ := json.Marshal(map[string]interface{}{
		"cmd": CmdFirstConnect,
		"ts":  float64(time.Now().Unix()),
		"params": map[string]interface{}{
			"machine_id": "live-m1", "hostname": "beacon-get-host",
		},
	})
	next, err := m.HandleBeaconFrame(sess, body)
	if err != nil {
		t.Fatalf("注册: %v", err)
	}
	sid := next.ID()

	// 旧命令帧排在 confirm 之后(模拟注册后服务端立即下发的命令)
	if err := sess.sendFrame([]byte(`{"cmd":"Ping","request_id":"old-1"}`)); err != nil {
		t.Fatalf("seed frame: %v", err)
	}

	// 第 1 次 GET(mid): 取到**注册 confirm**(队首, 非补发)
	frame := m.BeaconGet("10.0.0.9:4444", "live-m1", 0)
	if frame == nil || !strings.Contains(string(frame), "session_id") {
		t.Fatalf("第 1 帧应为注册 confirm, got %s", frame)
	}
	if got := confirmSID(t, frame); got != sid {
		t.Fatalf("confirm sid 应为 %d, got %d", sid, got)
	}
	// 第 2 次 GET(sid 态): 旧命令帧
	if frame2 := m.BeaconGet("10.0.0.9:4444", "", sid); frame2 == nil ||
		!strings.Contains(string(frame2), `"request_id":"old-1"`) {
		t.Fatalf("第 2 帧应为旧命令帧, got %s", frame2)
	}
	// 第 3 次 GET: 空(**不得再出现 confirm** —— 认领不补发)
	if frame3 := m.BeaconGet("10.0.0.9:4444", "live-m1", 0); frame3 != nil {
		t.Fatalf("认领不得补发 confirm, got %s", frame3)
	}
}

// TestBeaconGetMidNoRecord 新机器(无持久记录)mid 认领 → 无帧(nil),
// Agent 继续走 POST FirstConnect 注册路径 —— 认领路径绝不铸造会话。
func TestBeaconGetMidNoRecord(t *testing.T) {
	m := newTestManager()
	if frame := m.BeaconGet("10.0.0.9:4444", "unknown-machine", 0); frame != nil {
		t.Fatalf("无记录认领应返回 nil, got %s", frame)
	}
	if n := len(m.SessionInfos()); n != 0 {
		t.Fatalf("不得铸造会话, 内存表应有 0 项, got %d", n)
	}
}

// TestBeaconGetSidRouteAndFrame sid 态路由: 会话 outbox 的帧经 GET 取走(FIFO 单帧)。
func TestBeaconGetSidRouteAndFrame(t *testing.T) {
	m := newTestManager()
	sess, sid := registerBeacon(t, m, "sid-m1")

	if err := sess.sendFrame([]byte(`{"cmd":"Ping","request_id":"get-1"}`)); err != nil {
		t.Fatalf("sendFrame: %v", err)
	}
	frame := m.BeaconGet("10.0.0.9:4444", "", sid)
	if frame == nil || !strings.Contains(string(frame), `"request_id":"get-1"`) {
		t.Fatalf("GET 应取到命令帧, got %s", frame)
	}
	// 取空后返回 nil
	if frame := m.BeaconGet("10.0.0.9:4444", "", sid); frame != nil {
		t.Fatalf("outbox 空后应返回 nil, got %s", frame)
	}
}

// TestBeaconGetSidUnknownReregister 未知 sid → "重新注册"信号帧经 GET 响应体带回
// (POST 纯上行后, 这类信号只能走 GET; 帧必须回带服务端不认得的那个 sid 供 Agent 归属校验)。
func TestBeaconGetSidUnknownReregister(t *testing.T) {
	m := newTestManager()
	frame := m.BeaconGet("10.0.0.9:4444", "", 999)
	if frame == nil {
		t.Fatal("未知 sid 应返回重新注册帧")
	}
	var resp struct {
		Code      string `json:"code"`
		SessionID int    `json:"session_id"`
	}
	if err := json.Unmarshal(frame, &resp); err != nil {
		t.Fatalf("reregister 帧解析: %v", err)
	}
	if resp.Code != "SESSION_UNKNOWN" || resp.SessionID != 999 {
		t.Fatalf("应含 SESSION_UNKNOWN 且回带 sid=999, got %+v", resp)
	}
	// 且不得铸造会话
	if n := len(m.SessionInfos()); n != 0 {
		t.Fatalf("未知 sid 不得铸造会话, got %d", n)
	}
}

// TestBeaconGetConfirmBeforeLaterCommands S3.3 对偶面: 复用注册的 confirm 帧
// 先于其后派发的命令到达 Agent( 复用注册事故的顺序保证, 由 outbox FIFO 天然承载)。
// 
func TestBeaconGetConfirmBeforeLaterCommands(t *testing.T) {
	m := newTestManager()
	sess, sid := registerBeacon(t, m, "order-m1") // 注册 confirm 已被 helper 取走

	// 认领(内存活跃)不补发 → 第 1 次 GET 空
	if frame := m.BeaconGet("10.0.0.9:4444", "order-m1", 0); frame != nil {
		t.Fatalf("认领不得补发 confirm, got %s", frame)
	}

	// 认领之后派发的命令经下一次 GET 可见
	m.dispatchTask(sess, &store.Task{ID: 1, SessionID: sid, Cmd: "Ping", RequestID: "later-1"})
	frame2 := m.BeaconGet("10.0.0.9:4444", "", sid)
	if frame2 == nil || !strings.Contains(string(frame2), `"request_id":"later-1"`) {
		t.Fatalf("GET 应取到后派发的命令帧, got %s", frame2)
	}
}

// TestBeaconGetTaskDispatch 直写 outbox 的任务帧可被 GET 取走:
// dispatchTask 对 beacon 会话的分流(既有语义) + GET 取件(新增)拼接后的端到端保证。
func TestBeaconGetTaskDispatch(t *testing.T) {
	m := newTestManager()
	sess, sid := registerBeacon(t, m, "task-m1")

	task := &store.Task{ID: 1, SessionID: sid, Cmd: "Ping", RequestID: "task-1"}
	m.dispatchTask(sess, task)

	frame := m.BeaconGet("10.0.0.9:4444", "", sid)
	if frame == nil {
		t.Fatal("GET 应取到任务命令帧")
	}
	var resp struct {
		Cmd       string `json:"cmd"`
		RequestID string `json:"request_id"`
	}
	if err := json.Unmarshal(frame, &resp); err != nil {
		t.Fatalf("命令帧解析: %v", err)
	}
	if resp.Cmd != "Ping" || resp.RequestID != "task-1" {
		t.Fatalf("命令帧内容不符: %+v", resp)
	}
}

// TestDispatchAndWaitRequestIDMatch S3.5: 响应匹配唯一语义 —— dispatchAndWait 挂
// PendingResp, 带同 request_id 的响应帧经 handleOne 精确送达等待者。
// (原 RespChan FIFO 匹配已删, 此用例锁定合并后的服务端唯一响应通路。)
func TestDispatchAndWaitRequestIDMatch(t *testing.T) {
	m := newTestManager()
	sess, _ := registerBeacon(t, m, "match-m1")

	type result struct {
		resp map[string]interface{}
		err  error
	}
	done := make(chan result, 1)
	go func() {
		r, e := sess.SendCommand("Ping", map[string]interface{}{"n": 1})
		done <- result{r, e}
	}()

	// 等命令帧入 outbox(cmdMu 由等待者持有, 取帧不受影响)
	var cmdFrame []byte
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		if f, ok := sess.PollOutbox(); ok {
			cmdFrame = f
			break
		}
		time.Sleep(5 * time.Millisecond)
	}
	if cmdFrame == nil {
		t.Fatal("命令帧应入 outbox")
	}
	var cmd struct {
		RequestID string `json:"request_id"`
		Cmd       string `json:"cmd"`
	}
	if err := json.Unmarshal(cmdFrame, &cmd); err != nil || cmd.RequestID == "" || cmd.Cmd != "Ping" {
		t.Fatalf("命令帧不符: %s", cmdFrame)
	}

	// 模拟 Agent 回响应(带同 rid) → handleOne 应把它送达等待者
	respBody := fmt.Sprintf(`{"status":"success","request_id":%q,"ts":%d}`,
		cmd.RequestID, time.Now().Unix())
	if next, _ := sess.handleOne([]byte(respBody), m); next != sess {
		t.Fatal("响应帧应由本会话处理")
	}

	r := <-done
	if r.err != nil {
		t.Fatalf("dispatchAndWait: %v", r.err)
	}
	if r.resp["status"] != "success" {
		t.Fatalf("响应内容不符: %+v", r.resp)
	}
	// 响应送达后 PendingResp 应清理干净
	sess.pendingMu.Lock()
	n := len(sess.PendingResp)
	sess.pendingMu.Unlock()
	if n != 0 {
		t.Fatalf("PendingResp 应清理干净, 剩 %d", n)
	}
}
