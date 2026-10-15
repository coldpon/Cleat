package session

import (
	"testing"
	"time"
)

// TestBeaconOutboxCap 拒收而非丢队首(A3-14): outbox 满 → sendFrame 报错;
// 已有帧原样保留且 FIFO 队首(可能是注册 confirm)不被挤掉。
func TestBeaconOutboxCap(t *testing.T) {
	m := newTestManager()
	sess := m.NewBeaconSession("1.2.3.4:555")

	if err := sess.sendFrame([]byte("confirm-head")); err != nil {
		t.Fatalf("队首帧: %v", err)
	}
	for i := 1; i < MaxOutbox; i++ {
		if err := sess.sendFrame([]byte("fill")); err != nil {
			t.Fatalf("填充第 %d 帧不该失败: %v", i, err)
		}
	}
	if err := sess.sendFrame([]byte("overflow")); err == nil {
		t.Fatal("超限帧应被拒收")
	}
	// 队首必须原样: 若实现改成丢最老, confirm 丢失 = Agent 永远拿不到 sid
	if f, ok := sess.PollOutbox(); !ok || string(f) != "confirm-head" {
		t.Fatalf("队首应保留 confirm-head, got %q ok=%v", f, ok)
	}
}

// TestSweepStaleBeacons 停拍判离线(A3-13): 静默超阈值 → Closed=true 并移出
// 会话表; 新鲜 beacon 与 TCP 会话不受影响。
func TestSweepStaleBeacons(t *testing.T) {
	m := newTestManager()

	stale := m.NewBeaconSession("1.2.3.4:555")
	if err := m.registerForTest(stale, "mid-stale"); err != nil {
		t.Fatalf("register stale: %v", err)
	}
	fresh := m.NewBeaconSession("1.2.3.4:556")
	if err := m.registerForTest(fresh, "mid-fresh"); err != nil {
		t.Fatalf("register fresh: %v", err)
	}
	// 把 stale 的 LastSeen 拨回阈值之外
	stale.Record.Lock()
	stale.Record.LastSeen = time.Now().Add(-BeaconOfflineAfter - time.Minute)
	stale.Record.Unlock()

	n := m.SweepStaleBeacons()
	if n != 1 {
		t.Fatalf("应标记 1 个, got %d", n)
	}
	if !stale.Record.Closed {
		t.Fatal("停拍会话应已标离线")
	}
	if m.sessionByID(stale.ID()) != nil {
		t.Fatal("停拍会话应已移出会话表")
	}
	if fresh.Record.Closed {
		t.Fatal("新鲜 beacon 会话不应被误杀")
	}
	if m.sessionByID(fresh.ID()) == nil {
		t.Fatal("新鲜 beacon 会话应仍在表中")
	}
}

// registerForTest 直接把会话注册进 Manager(绕过帧解析, 只测 sweeper 语义)。
func (m *Manager) registerForTest(s *Session, machineID string) error {
	id := m.st.NewID()
	s.Record.ID = id
	s.Record.MachineID = machineID
	s.Record.LastSeen = time.Now()
	m.st.Put(s.Record)
	m.mu.Lock()
	m.sessions[id] = s
	m.mu.Unlock()
	return nil
}
