package session

import (
	"testing"
	"time"

	"cleat/teamserver/internal/eventbus"
	"cleat/teamserver/internal/store"
)

// TestIngestAllowedWindow 限流语义锁定: 窗口内第 11 帧起拒绝, 窗口滑过恢复,
// 键间隔离, TTL 过期清理(离线会话防泄漏, A1-1 教训)。
func TestIngestAllowedWindow(t *testing.T) {
	ingestMu.Lock()
	savedHits, savedWarn := ingestHits, ingestWarn
	ingestHits = map[string][]time.Time{}
	ingestWarn = map[string]time.Time{}
	ingestMu.Unlock()
	t.Cleanup(func() {
		ingestMu.Lock()
		ingestHits, ingestWarn = savedHits, savedWarn
		ingestMu.Unlock()
	})

	base := time.Now()
	key := "s:42"
	for i := 0; i < beaconIngestLimit; i++ {
		if !ingestAllowed(key, base.Add(time.Duration(i)*10*time.Millisecond)) {
			t.Fatalf("第 %d 帧应放行(限额 %d)", i+1, beaconIngestLimit)
		}
	}
	if ingestAllowed(key, base.Add(200*time.Millisecond)) {
		t.Fatal("窗口内超限帧应拒绝")
	}
	// 其他键独立配额
	if !ingestAllowed("s:43", base.Add(200*time.Millisecond)) {
		t.Fatal("不同会话不应共享配额")
	}
	// 窗口滑过: 恢复放行
	if !ingestAllowed(key, base.Add(1100*time.Millisecond)) {
		t.Fatal("窗口滑过后应重新放行")
	}
}

// TestIngestKeyTTL 清理语义: 键超 TTL 无命中后, 下一次触及即整体删除(含告警条目)。
func TestIngestKeyTTL(t *testing.T) {
	ingestMu.Lock()
	savedHits, savedWarn := ingestHits, ingestWarn
	ingestHits = map[string][]time.Time{}
	ingestWarn = map[string]time.Time{}
	ingestMu.Unlock()
	t.Cleanup(func() {
		ingestMu.Lock()
		ingestHits, ingestWarn = savedHits, savedWarn
		ingestMu.Unlock()
	})

	base := time.Now()
	key := "ip:10.0.0.9"
	if !ingestAllowed(key, base) {
		t.Fatal("首帧应放行")
	}
	ingestMu.Lock()
	ingestWarn[key] = base // 模拟曾告警
	ingestMu.Unlock()

	// TTL(10min) 过后触及: 旧条目应被整体清理, 本次视为全新配额
	if !ingestAllowed(key, base.Add(ingestKeyTTL+time.Minute)) {
		t.Fatal("TTL 过后的帧应按全新配额放行")
	}
	ingestMu.Lock()
	n := len(ingestHits[key])
	ingestMu.Unlock()
	if n != 1 {
		t.Fatalf("TTL 清理后应只余本次命中 1 条, got %d", n)
	}
}

// TestBeaconFrameRateLimited Manager 级: 同 sid 帧率超限后, BeaconFrame 返回 (nil, nil)
// —— 与"TCP 冲突忽略本帧"同形态, 调用方零改动; 未知 sid 的正常路径本会返回重注册信号帧,
// 超限后连信号帧也不发(静默丢弃)。
func TestBeaconFrameRateLimited(t *testing.T) {
	m := NewManager(store.New(), eventbus.New(), nil, nil, false)
	sid := 98765
	// 未知 sid 的前 beaconIngestLimit 帧: 走"未知会话"分支, 返回重注册信号帧(非 nil)
	for i := 0; i < beaconIngestLimit; i++ {
		next, err := m.BeaconFrame("10.9.9.9:1234", sid, []byte(`{"t":1}`))
		if err != nil || next == nil {
			t.Fatalf("第 %d 帧应走未知会话分支(非 nil), err=%v", i+1, err)
		}
	}
	// 超限: 限流层直接丢弃
	next, err := m.BeaconFrame("10.9.9.9:1234", sid, []byte(`{"t":2}`))
	if err != nil {
		t.Fatalf("限流丢弃不应返回错误(绝不回错误给 Agent), err=%v", err)
	}
	if next != nil {
		t.Fatal("超限帧应被静默丢弃(nil)")
	}
}

// TestBeaconGetRateLimited GET 侧: 超限返回空(nil) = "无帧"语义, Agent 无感。
func TestBeaconGetRateLimited(t *testing.T) {
	m := NewManager(store.New(), eventbus.New(), nil, nil, false)
	sid := 98766
	for i := 0; i < beaconIngestLimit; i++ {
		_ = m.BeaconGet("10.9.9.10:1234", "", sid) // 未知 sid → 重注册信号帧, 正常消耗配额
	}
	if f := m.BeaconGet("10.9.9.10:1234", "", sid); f != nil {
		t.Fatal("超限 GET 应返回空(nil = 无帧)")
	}
}
