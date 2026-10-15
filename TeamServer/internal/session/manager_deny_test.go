package session

import (
	"encoding/json"
	"testing"
	"time"

	"cleat/teamserver/internal/store"
)

// TestRegisterDebouncePersistentDenyAllowsTakeover 死锁破除回归:
// 旧实例死亡后, 重启的新实例以同 machine_id 走身份路由 GET 持续刷新旧记录 LastSeen,
// "仍活跃"永真 → 永久 DENY_BUSY(实测 >20 分钟, 设计窗口只有 2 分钟; 全新启动 sid=0
// 不走自接管豁免)。修复: 同 machine_id 的注册被连续拒绝满一个完整防抖窗 → 允许接管。
// 同时钉住 S4 保护: 窗内的重复注册仍被拒绝(不因本修复放开)。
//
// 断言形态: HandleBeaconFrame 吞掉 registerNew 的错误,
// 拒绝以 pending 会话 outbox 里的 DENY_BUSY 帧呈现; 接管成功则返回旧会话。
func TestRegisterDebouncePersistentDenyAllowsTakeover(t *testing.T) {
	old := reRegisterDebounce
	reRegisterDebounce = 30 * time.Millisecond
	defer func() { reRegisterDebounce = old }()

	m := newTestManager()
	frame := func() []byte {
		t.Helper()
		body, _ := json.Marshal(map[string]interface{}{
			"cmd": CmdFirstConnect,
			"ts":  float64(time.Now().Unix()),
			"params": map[string]interface{}{
				"machine_id": "m-dl", "hostname": "h",
			},
		})
		return body
	}
	assertDenied := func(t *testing.T, s *Session, tag string) {
		t.Helper()
		f, ok := s.PollOutbox()
		if !ok {
			t.Fatalf("[%s] 应下发 DENY_BUSY, 但 outbox 为空", tag)
		}
		var resp map[string]interface{}
		if err := json.Unmarshal(f, &resp); err != nil {
			t.Fatalf("[%s] deny 帧解析失败: %v", tag, err)
		}
		if resp["code"] != "DENY_BUSY" {
			t.Fatalf("[%s] 应为 DENY_BUSY, got %v", tag, resp)
		}
	}
	touch := func(rec *store.Record) {
		rec.Lock()
		rec.LastSeen = time.Now()
		rec.Unlock()
	}

	// 旧实例注册
	a := m.NewBeaconSession("10.0.0.8:1")
	if _, err := m.HandleBeaconFrame(a, frame()); err != nil {
		t.Fatalf("A register: %v", err)
	}
	rec := m.st.ByMachine("m-dl")
	if rec == nil {
		t.Fatal("A record missing")
	}

	// 新实例窗内尝试 → 防抖拒绝 + 开始计时; Touch 模拟身份路由 GET 刷新(死锁前提)
	b := m.NewBeaconSession("10.0.0.8:2")
	touch(rec)
	if _, err := m.HandleBeaconFrame(b, frame()); err != nil {
		t.Fatalf("B: %v", err)
	}
	assertDenied(t, b, "窗内首次")

	// 持续拒绝未满一窗 → 仍拒绝(S4 保护保留)
	time.Sleep(10 * time.Millisecond)
	c := m.NewBeaconSession("10.0.0.8:3")
	touch(rec)
	if _, err := m.HandleBeaconFrame(c, frame()); err != nil {
		t.Fatalf("C: %v", err)
	}
	assertDenied(t, c, "窗内未满一窗")

	// 持续拒绝满一窗(死锁形态) → 允许接管: 返回旧会话, 确认帧含同一 session_id
	time.Sleep(35 * time.Millisecond)
	d := m.NewBeaconSession("10.0.0.8:4")
	touch(rec)
	next, err := m.HandleBeaconFrame(d, frame())
	if err != nil {
		t.Fatalf("持续拒绝满一窗应允许接管: %v", err)
	}
	if next == nil || next.Record.ID != rec.ID {
		t.Fatalf("接管应复用会话 #%d, got %+v", rec.ID, next)
	}
	f, ok := next.PollOutbox()
	if !ok {
		t.Fatal("接管应下发确认帧")
	}
	var confirm map[string]interface{}
	if err := json.Unmarshal(f, &confirm); err != nil {
		t.Fatalf("确认帧解析失败: %v", err)
	}
	if id, _ := confirm["session_id"].(float64); int(id) != rec.ID {
		t.Fatalf("确认帧 session_id 应为 %d, got %v", rec.ID, confirm)
	}

	// 接管后 denySince 应已清除(防抖纠纷了结; 再拒绝计时从零开始)
	m.mu.Lock()
	_, leaked := m.denySince[rec.ID]
	m.mu.Unlock()
	if leaked {
		t.Fatal("接管后 denySince 应清除")
	}
}
