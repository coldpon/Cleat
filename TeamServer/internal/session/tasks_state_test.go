package session

// 第 3 批状态机收口测试。
// RespTimeout 已 var 化: 测试统一缩为毫秒级(180s 无法在测试里等), defer 恢复。

import (
	"testing"
	"time"

	"cleat/teamserver/internal/eventbus"
	"cleat/teamserver/internal/store"
)

// shortenRespTimeout 把响应等待窗缩到毫秒级, 返回恢复函数。
func shortenRespTimeout(t *testing.T) {
	t.Helper()
	saved := RespTimeout
	RespTimeout = 30 * time.Millisecond
	t.Cleanup(func() { RespTimeout = saved })
}

// 3x 窗耗尽 → **timeout 终态**(此前回写 dispatched 永久卡死)。
func TestTaskTimeoutAfterDelivery(t *testing.T) {
	shortenRespTimeout(t)
	m, _ := newTestManagerWithSession(t)

	id, err := m.SubmitTask(1, "ScreenShot", nil, "push")
	if err != nil {
		t.Fatalf("SubmitTask: %v", err)
	}
	// 3x 窗(90ms) + 余量
	waitFor(t, 500*time.Millisecond, func() bool {
		tk, _ := m.APITask(id)
		return tk.Status == store.TaskTimeout
	})
	tk, _ := m.APITask(id)
	if tk.Status != store.TaskTimeout {
		t.Fatalf("已交付超窗应 timeout, got %s (result=%q)", tk.Status, tk.Result)
	}
}

// TestTaskRollbackOnWithdraw 防双执行回归锁定: beacon 会话帧未取走 → 3x 窗耗尽撤回成功 →
// 回 **pending**(下轮 compensate 重派是唯一重派路径)。此语义曾在二审修正时引入双执行, 必须锁死。
func TestTaskRollbackOnWithdraw(t *testing.T) {
	shortenRespTimeout(t)
	m := NewManager(store.New(), eventbus.New(), nil, nil, false)

	// 注册一个 beacon 会话(无 Conn, 出站帧进 outbox)
	sess := New(nil, eventbus.New())
	sess.MarkBeacon()
	sess.Record = &store.Record{ID: 55, RemoteIP: "test", Closed: false}
	m.mu.Lock()
	m.sessions[55] = sess
	m.mu.Unlock()

	id, err := m.SubmitTask(55, "FileListDir", nil, "pull")
	if err != nil {
		t.Fatalf("SubmitTask: %v", err)
	}
	// 派发成功的证据 = dispatched 状态。**不可用 PollOutbox 验证帧存在** —— 它有取走
	// 副作用, 取走后撤回必然失败(Remove=false), 测试会自相矛盾。
	if tk, _ := m.APITask(id); tk.Status != store.TaskDispatched {
		t.Fatalf("派发后应 dispatched, got %s", tk.Status)
	}
	// 全程无人取件: 3x 窗后帧仍在 outbox → Remove=true → 撤回成功 → 回 pending
	waitFor(t, 500*time.Millisecond, func() bool {
		tk, _ := m.APITask(id)
		return tk.Status == store.TaskPending
	})
	if tk, _ := m.APITask(id); tk.Status != store.TaskPending {
		t.Fatalf("未取走帧超窗应回 pending, got %s", tk.Status)
	}
}

// 不同参数不取代; 终态任务不取代。
func TestSupersedeDuplicates(t *testing.T) {
	m := NewManager(store.New(), eventbus.New(), nil, nil, false)
	sess := New(nil, eventbus.New())
	sess.MarkBeacon()
	sess.Record = &store.Record{ID: 66, RemoteIP: "test", Closed: false}
	m.mu.Lock()
	m.sessions[66] = sess
	m.mu.Unlock()

	// 第一笔: dispatched
	id1, _ := m.SubmitTask(66, "Shell", map[string]interface{}{"c": "whoami"}, "pull")
	if tk, _ := m.APITask(id1); tk.Status != store.TaskDispatched {
		t.Fatalf("首笔应 dispatched, got %s", tk.Status)
	}
	// 重复提交同命令同参数: 旧任务让位
	id2, _ := m.SubmitTask(66, "Shell", map[string]interface{}{"c": "whoami"}, "pull")
	tk1, _ := m.APITask(id1)
	if tk1.Status != store.TaskSuperseded {
		t.Fatalf("旧任务应 superseded, got %s (result=%q)", tk1.Status, tk1.Result)
	}
	if tk2, _ := m.APITask(id2); tk2.Status != store.TaskDispatched {
		t.Fatalf("新任务应 dispatched, got %s", tk2.Status)
	}
	// 旧任务的帧应被撤回: outbox 里只剩新任务的帧
	if _, ok := sess.PollOutbox(); !ok {
		t.Fatal("outbox 应仍有帧(新任务的)")
	}
	if _, ok := sess.PollOutbox(); ok {
		t.Fatal("outbox 应恰有一帧(旧任务帧已被撤回)")
	}
	// 不同参数: 不取代
	id3, _ := m.SubmitTask(66, "Shell", map[string]interface{}{"c": "id"}, "pull")
	if tk2, _ := m.APITask(id2); tk2.Status != store.TaskDispatched {
		t.Fatalf("不同参数不应取代, 旧任务应为 dispatched, got %s", tk2.Status)
	}
	// 终态任务不取代: 把 id3 标终态再重发同参数
	m.st.UpdateTaskStatus(id3, store.TaskCompleted, `{}`)
	id4, _ := m.SubmitTask(66, "Shell", map[string]interface{}{"c": "id"}, "pull")
	if tk3, _ := m.APITask(id3); tk3.Status != store.TaskCompleted {
		t.Fatalf("终态任务不应被取代, got %s", tk3.Status)
	}
	if tk4, _ := m.APITask(id4); tk4.Status != store.TaskDispatched {
		t.Fatalf("重发的新任务应正常派发, got %s", tk4.Status)
	}
}

func TestLateResponseRevives(t *testing.T) {
	m := NewManager(store.New(), eventbus.New(), nil, nil, false)

	// timeout 任务 → 复活为 completed
	id1, _ := m.st.SaveTask(&store.Task{SessionID: 1, Cmd: "Shell", RequestID: "req-late-1", Status: store.TaskTimeout})
	m.reviveLateResponse("req-late-1", []byte(`{"status":"success"}`))
	if tk, _ := m.APITask(id1); tk.Status != store.TaskCompleted {
		t.Fatalf("timeout 应复活为 completed, got %s", tk.Status)
	}
	// superseded 任务: 晚到响应作废
	id2, _ := m.st.SaveTask(&store.Task{SessionID: 1, Cmd: "Shell", RequestID: "req-late-2", Status: store.TaskSuperseded})
	m.reviveLateResponse("req-late-2", []byte(`{"status":"success"}`))
	if tk, _ := m.APITask(id2); tk.Status != store.TaskSuperseded {
		t.Fatalf("superseded 不应复活, got %s", tk.Status)
	}
	// dispatched 任务(等待 goroutine 在途): 复活分支绝不触碰(双写竞态防护)
	id3, _ := m.st.SaveTask(&store.Task{SessionID: 1, Cmd: "Shell", RequestID: "req-late-3", Status: store.TaskDispatched})
	m.reviveLateResponse("req-late-3", []byte(`{"status":"success"}`))
	if tk, _ := m.APITask(id3); tk.Status != store.TaskDispatched {
		t.Fatalf("dispatched 不应经复活分支改动, got %s", tk.Status)
	}
	// 并发复活唯一赢家: timeout → 两个响应并发, 恰一个生效(结果必为终态之一)
	id4, _ := m.st.SaveTask(&store.Task{SessionID: 1, Cmd: "Shell", RequestID: "req-late-4", Status: store.TaskTimeout})
	done := make(chan struct{}, 2)
	for i := 0; i < 2; i++ {
		go func() {
			m.reviveLateResponse("req-late-4", []byte(`{"status":"ok"}`))
			done <- struct{}{}
		}()
	}
	<-done
	<-done
	if tk, _ := m.APITask(id4); tk.Status != store.TaskCompleted {
		t.Fatalf("并发复活后应为 completed(恰一赢家), got %s", tk.Status)
	}
}

// 非 dispatched 状态不碰。
func TestSweepStuckTasks(t *testing.T) {
	shortenRespTimeout(t) // cutoff = 10×30ms = 300ms
	m := NewManager(store.New(), eventbus.New(), nil, nil, false)

	idOld, _ := m.st.SaveTask(&store.Task{SessionID: 1, Cmd: "Old", Status: store.TaskDispatched})
	idFresh, _ := m.st.SaveTask(&store.Task{SessionID: 1, Cmd: "Fresh", Status: store.TaskDispatched})
	idDone, _ := m.st.SaveTask(&store.Task{SessionID: 1, Cmd: "Done", Status: store.TaskCompleted})

	time.Sleep(350 * time.Millisecond) // 让 Old/Fresh 超龄(Fresh 随后刷新)
	// Fresh 刷新 UpdatedAt 到当前 → 未超龄
	m.st.UpdateTaskStatusIf(idFresh, store.TaskDispatched, store.TaskDispatched, "touch")

	if n := m.SweepStuckTasks(); n != 1 {
		t.Fatalf("应恰好扫到 1 个僵尸, got %d", n)
	}
	if tk, _ := m.APITask(idOld); tk.Status != store.TaskTimeout {
		t.Fatalf("超龄 dispatched 应被置 timeout, got %s", tk.Status)
	}
	if tk, _ := m.APITask(idFresh); tk.Status != store.TaskDispatched {
		t.Fatalf("窗口内 dispatched 不应被扫, got %s", tk.Status)
	}
	if tk, _ := m.APITask(idDone); tk.Status != store.TaskCompleted {
		t.Fatalf("终态任务不应被扫, got %s", tk.Status)
	}
}

func TestPayloadResolverError(t *testing.T) {
	m, _ := newTestManagerWithSession(t)
	m.SetPayloadResolver(func(sid int, cmd string, params map[string]interface{}) ([]byte, error) {
		return nil, errTestNoPayload
	})
	id, err := m.SubmitTask(1, "ModuleLoad", map[string]interface{}{"module_id": "x"}, "pull")
	if err != nil {
		t.Fatalf("SubmitTask 本身不应失败(异步校验在派发期): %v", err)
	}
	waitFor(t, 500*time.Millisecond, func() bool {
		tk, _ := m.APITask(id)
		return tk.Status == store.TaskFailed
	})
	tk, _ := m.APITask(id)
	if tk.Status != store.TaskFailed {
		t.Fatalf("resolver 失败应 failed, got %s", tk.Status)
	}
}

func TestPayloadResolverFrame(t *testing.T) {
	m := NewManager(store.New(), eventbus.New(), nil, nil, false)
	sess := New(nil, eventbus.New())
	sess.MarkBeacon()
	sess.Record = &store.Record{ID: 77, RemoteIP: "test", Closed: false}
	m.mu.Lock()
	m.sessions[77] = sess
	m.mu.Unlock()
	m.SetPayloadResolver(func(sid int, cmd string, params map[string]interface{}) ([]byte, error) {
		return []byte("BINARY_PAYLOAD"), nil
	})

	if _, err := m.SubmitTask(77, "ModuleLoad", map[string]interface{}{"module_id": "m1"}, "pull"); err != nil {
		t.Fatalf("SubmitTask: %v", err)
	}
	f, ok := sess.PollOutbox()
	if !ok {
		t.Fatal("outbox 应有派发帧")
	}
	// 帧形态: JSON 头 + '\0' + 二进制体
	head, bin, ok2 := splitHybrid(f)
	if !ok2 {
		t.Fatalf("应为大载荷混合帧, got %q", f[:min(40, len(f))])
	}
	if string(bin) != "BINARY_PAYLOAD" {
		t.Fatalf("二进制体不符, got %q", bin)
	}
	if !containsStr(string(head), `"body_size":14`) && !containsStr(string(head), `"body_size": 14`) {
		t.Fatalf("JSON 头应含 body_size, got %s", head)
	}
}

// 取消信号退出, 晚到的超时回写被 CAS 拦截(不覆盖 superseded)。
func TestSupersedeCancelsWaiter(t *testing.T) {
	shortenRespTimeout(t) // 3x 窗 = 90ms
	m := NewManager(store.New(), eventbus.New(), nil, nil, false)
	sess := New(nil, eventbus.New())
	sess.MarkBeacon()
	sess.Record = &store.Record{ID: 88, RemoteIP: "test", Closed: false}
	m.mu.Lock()
	m.sessions[88] = sess
	m.mu.Unlock()

	id1, _ := m.SubmitTask(88, "Shell", map[string]interface{}{"c": "a"}, "pull")
	// 立即重发同命令(旧任务 dispatched 且等待 goroutine 在途): 取代
	id2, _ := m.SubmitTask(88, "Shell", map[string]interface{}{"c": "a"}, "pull")
	if tk, _ := m.APITask(id1); tk.Status != store.TaskSuperseded {
		t.Fatalf("旧任务应立即 superseded, got %s", tk.Status)
	}
	// 旧等待 goroutine 的 3x 窗很快到点: 若取消失效, 它会走超时分支 CAS(失败, 不覆盖)
	time.Sleep(200 * time.Millisecond)
	if tk, _ := m.APITask(id1); tk.Status != store.TaskSuperseded {
		t.Fatalf("取消+CAS 应保住 superseded 终态, got %s", tk.Status)
	}
	// 新任务帧无人取走(beacon 无 GET) → 3x 窗后撤回回滚 pending —— 这是正确的回滚语义
	tk2, _ := m.APITask(id2)
	if tk2.Status != store.TaskPending {
		t.Fatalf("新任务帧未取走应回 pending(回滚语义), got %s (result=%q)", tk2.Status, tk2.Result)
	}
}

// 会话上线 compensate → resolver 取字节 → dispatched 且帧携带二进制体。
func TestModuleLoadCompensationCarriesPayload(t *testing.T) {
	m := NewManager(store.New(), eventbus.New(), nil, nil, false)
	m.SetPayloadResolver(func(sid int, cmd string, params map[string]interface{}) ([]byte, error) {
		return []byte("DLL_BYTES"), nil
	})

	// 离线提交(会话不存在): pending
	id, err := m.SubmitTask(99, "ModuleLoad", map[string]interface{}{"module_id": "m1"}, "pull")
	if err != nil {
		t.Fatalf("SubmitTask: %v", err)
	}
	if tk, _ := m.APITask(id); tk.Status != store.TaskPending {
		t.Fatalf("离线提交应 pending, got %s", tk.Status)
	}

	// 会话上线: compensate 补偿派发
	sess := New(nil, eventbus.New())
	sess.MarkBeacon()
	sess.Record = &store.Record{ID: 99, RemoteIP: "test", Closed: false}
	m.mu.Lock()
	m.sessions[99] = sess
	m.mu.Unlock()
	m.compensate(sess)

	if tk, _ := m.APITask(id); tk.Status != store.TaskDispatched {
		t.Fatalf("补偿后应 dispatched, got %s", tk.Status)
	}
	f, ok := sess.PollOutbox()
	if !ok {
		t.Fatal("补偿后 outbox 应有帧")
	}
	head, bin, isHybrid := splitHybrid(f)
	if !isHybrid || string(bin) != "DLL_BYTES" {
		t.Fatalf("补偿帧应携带 resolver 字节, hybrid=%v bin=%q", isHybrid, bin)
	}
	if !containsStr(string(head), `"module_id":"m1"`) {
		t.Fatalf("帧头应含 module_id, got %s", head)
	}
}

// ---- 小工具 ----

var errTestNoPayload = &testErr{"module gone"}

type testErr struct{ s string }

func (e *testErr) Error() string { return e.s }

// waitFor 轮询直到 cond 为真或超时(状态机异步回写的等待原语)。
func waitFor(t *testing.T, d time.Duration, cond func() bool) {
	t.Helper()
	deadline := time.Now().Add(d)
	for time.Now().Before(deadline) {
		if cond() {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
}

func splitHybrid(frame []byte) (head []byte, body []byte, ok bool) {
	for i, b := range frame {
		if b == 0 {
			return frame[:i], frame[i+1:], true
		}
	}
	return nil, nil, false
}

func containsStr(s, sub string) bool {
	return len(sub) == 0 || (len(s) >= len(sub) && indexOf(s, sub) >= 0)
}

func indexOf(s, sub string) int {
	for i := 0; i+len(sub) <= len(s); i++ {
		if s[i:i+len(sub)] == sub {
			return i
		}
	}
	return -1
}

func min(a, b int) int {
	if a < b {
		return a
	}
	return b
}
