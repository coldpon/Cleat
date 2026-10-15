package session

import (
	"encoding/json"
	"strings"
	"testing"
	"time"

	"cleat/teamserver/internal/crypto"
	"cleat/teamserver/internal/eventbus"
	"cleat/teamserver/internal/store"
)

// testCrypto 生成测试用 Box/X25519Box。
func testCrypto() (*crypto.Box, *crypto.X25519Box) {
	cbox, _ := crypto.New(strings.Repeat("ab", 32)) // 64 hex 字符 psk
	_, privHex, _ := crypto.GenerateX25519KeyPair()
	xbox, _ := crypto.NewX25519(privHex)
	return cbox, xbox
}

// newTestManager 用默认密钥构建 Manager(测试专用)。
func newTestManager() *Manager {
	cbox, xbox := testCrypto()
	return NewManager(store.New(), eventbus.New(), cbox, xbox, false)
}

// TestBeaconOutboxFIFO 验证 beacon 会话 sendFrame 进 outbox, PollOutbox 逐条 FIFO 取出。
func TestBeaconOutboxFIFO(t *testing.T) {
	m := newTestManager()
	sess := m.NewBeaconSession("1.2.3.4:555")

	if err := sess.sendFrame([]byte("first")); err != nil {
		t.Fatalf("sendFrame1: %v", err)
	}
	if err := sess.sendFrame([]byte("second")); err != nil {
		t.Fatalf("sendFrame2: %v", err)
	}
	if n := sess.OutboxLen(); n != 2 {
		t.Fatalf("outbox 应有 2 帧, got %d", n)
	}
	if f, ok := sess.PollOutbox(); !ok || string(f) != "first" {
		t.Fatalf("第1帧应为 first, got %q ok=%v", f, ok)
	}
	if f, ok := sess.PollOutbox(); !ok || string(f) != "second" {
		t.Fatalf("第2帧应为 second, got %q ok=%v", f, ok)
	}
	if _, ok := sess.PollOutbox(); ok {
		t.Fatal("空 outbox 不应取出")
	}
}

// TestBeaconHandleRegisters 验证 beacon 首帧(FirstConnect)经 HandleBeaconFrame 完成注册,
// 确认帧(含 session_id)入 outbox 可被 Agent 拉取。
func TestBeaconHandleRegisters(t *testing.T) {
	m := newTestManager()
	sess := m.NewBeaconSession("10.0.0.8:444")

	body, _ := json.Marshal(map[string]interface{}{
		"cmd": CmdFirstConnect,
		"ts":  float64(time.Now().Unix()),
		"params": map[string]interface{}{
			"machine_id": "beacon-m1", "hostname": "beacon-host",
		},
	})
	next, err := m.HandleBeaconFrame(sess, body)
	if err != nil {
		t.Fatalf("HandleBeaconFrame: %v", err)
	}
	if next == nil {
		t.Fatal("返回会话不应为 nil")
	}

	// 确认帧应已在 outbox(confirmSession 分流到 outbox)
	f, ok := next.PollOutbox()
	if !ok {
		t.Fatal("注册确认帧应入 outbox")
	}
	var resp map[string]interface{}
	if err := json.Unmarshal(f, &resp); err != nil {
		t.Fatalf("确认帧解析失败: %v", err)
	}
	if id, _ := resp["session_id"].(float64); int(id) == 0 {
		t.Fatalf("确认帧应含 session_id, got %v", resp)
	}

	// 会话已注册
	sid := int(resp["session_id"].(float64))
	if s := m.sessionByID(sid); s == nil {
		t.Fatal("beacon 会话应注册到 Manager")
	}
}

// jsonFrame 构造一帧明文 JSON(spy/回归用, 自动注入 ts 以过 checkReplay)。
func jsonFrame(t *testing.T, fields map[string]interface{}) []byte {
	t.Helper()
	fields["ts"] = float64(time.Now().Unix())
	body, err := json.Marshal(fields)
	if err != nil {
		t.Fatalf("jsonFrame: %v", err)
	}
	return body
}

// assertReregisterSignal 断言会话 outbox 首帧为"请重新注册"(SESSION_UNKNOWN)。
func assertReregisterSignal(t *testing.T, s *Session) {
	t.Helper()
	f, ok := s.PollOutbox()
	if !ok {
		t.Fatal("应下发重新注册信号, 但 outbox 为空")
	}
	var sig map[string]interface{}
	if err := json.Unmarshal(f, &sig); err != nil {
		t.Fatalf("信号帧解析失败: %v", err)
	}
	if sig["code"] != "SESSION_UNKNOWN" {
		t.Fatalf("信号应为 SESSION_UNKNOWN, got %v", sig)
	}
}

// TestBeaconRestartDoesNotMintAnonymousSessions 回归: 服务端重启后, 已注册 Agent 的
// 在途请求不得被当成"新注册"而铸造匿名会话。
//
// 故障原景: 断开 → 重启(正常重连) → 再断开等一会 → 再重启, 前端疯狂刷新"未注册"消息, 差点打爆服务端。
// 根因: `Manager.sessions` 是**内存**表(`NewManager` 建空 map, 启动时不从 store 回填), 而
// `BeaconFrame` 把"sid 不在内存"直接当作"新注册" → 已注册 Agent 的**每个**在途请求都铸造一个
// 匿名会话(无 machine_id, 永不归并)。
//
// 判别力: 修复前必失败 —— 旧路径会 NewBeaconSession → ResolveIdentity 非注册分支 → registerAnonymous,
// 会话数从 1 涨到 2 且返回会话 ID 变成新号。
func TestBeaconRestartDoesNotMintAnonymousSessions(t *testing.T) {
	m := newTestManager()

	// 1. 正常注册一个 beacon 会话(带 machine_id, 落 store)
	sess := m.NewBeaconSession("10.0.0.9:444")
	next, err := m.HandleBeaconFrame(sess, jsonFrame(t, map[string]interface{}{
		"cmd":    CmdFirstConnect,
		"params": map[string]interface{}{"machine_id": "restart-m1", "hostname": "restart-host"},
	}))
	if err != nil {
		t.Fatalf("注册失败: %v", err)
	}
	f, ok := next.PollOutbox()
	if !ok {
		t.Fatal("注册确认帧应入 outbox")
	}
	var confirm map[string]interface{}
	_ = json.Unmarshal(f, &confirm)
	sid := int(confirm["session_id"].(float64))
	if sid == 0 {
		t.Fatalf("确认帧应含 session_id, got %v", confirm)
	}
	if n := len(m.sessions); n != 1 {
		t.Fatalf("注册后应有 1 个会话, got %d", n)
	}

	// 2. 模拟服务端重启: 内存表清空(持久记录/cleat.db 存活)
	m.mu.Lock()
	m.sessions = make(map[int]*Session)
	m.mu.Unlock()

	// 3. 已注册 Agent 的在途 Poll 帧(仍带旧 sid)到达
	got, err := m.BeaconFrame("10.0.0.9:444", sid,
		jsonFrame(t, map[string]interface{}{"cmd": CmdPoll, "session_id": sid}))
	if err != nil {
		t.Fatalf("BeaconFrame: %v", err)
	}

	// 4. 必须"复活"原会话: ID 不变、会话数不增
	if got == nil || got.ID() != sid {
		t.Fatalf("应复活原会话 #%d, got %v", sid, got)
	}
	if n := len(m.sessions); n != 1 {
		t.Fatalf("重启后不得铸造新会话, 会话数应为 1, got %d", n)
	}
	if rec := m.st.Get(sid); rec == nil || rec.MachineID != "restart-m1" {
		t.Fatalf("持久记录应保持 machine_id=restart-m1, got %+v", rec)
	}
	// 不得对该会话下发"重新注册"(它已被透明复活); 注意 outbox 里此前的 poll 应答是正常的
	for {
		frame, ok := got.PollOutbox()
		if !ok {
			break
		}
		var m map[string]interface{}
		if err := json.Unmarshal(frame, &m); err != nil {
			continue
		}
		if m["code"] == "SESSION_UNKNOWN" {
			t.Fatalf("复活路径不应要求重新注册, 却收到 %q", frame)
		}
	}
}

// TestBeaconRestartReregistrationStaleSid 回归: 服务端重启后, Agent 察觉断链→重新注册
// (重发 FirstConnect), 该帧**顶层带着 FrameBuilder 无条件注入的旧 session_id**。
//
// 这是"第一次重启正常"所依赖的路径, 不得被 sid 路由截断:
// 若按旧 sid 路由到"复活", handleOne 会因 Record.ID!=0 跳过身份解析 → 拿不到确认
// → Agent 8s 确认超时 → 断链重连 → 重新注册又撞同一条路 → **永不符**的死循环(比原 bug 更糟)。
// 故注册帧必须走 pending 身份解析(与 TCP 侧 ResolveIdentity 只看 cmd 的行为一致)。
//
// 判别力: 去掉 isRegistrationFrame 后必失败 —— 没有确认帧, 且 FirstConnect 被当成未知主动消息。
func TestBeaconRestartReregistrationStaleSid(t *testing.T) {
	m := newTestManager()

	// 1. 注册(拿到 sid)
	next, err := m.HandleBeaconFrame(m.NewBeaconSession("10.0.0.11:444"),
		jsonFrame(t, map[string]interface{}{
			"cmd":    CmdFirstConnect,
			"params": map[string]interface{}{"machine_id": "restart-m2", "hostname": "h2"},
		}))
	if err != nil {
		t.Fatalf("注册失败: %v", err)
	}
	f, _ := next.PollOutbox()
	var confirm map[string]interface{}
	_ = json.Unmarshal(f, &confirm)
	sid := int(confirm["session_id"].(float64))
	if sid == 0 {
		t.Fatalf("确认帧应含 session_id, got %v", confirm)
	}

	// 2. 服务端重启: 内存表清空; 持久记录存活, 且按 loadFromPersistence 的语义
	//    **一律标记为离线**(store_persist.go: "历史会话初始视为离线, 等待 Agent 重连激活")
	rec := m.st.Get(sid)
	rec.Lock()
	rec.Closed = true
	rec.Unlock()
	m.mu.Lock()
	m.sessions = make(map[int]*Session)
	m.mu.Unlock()

	// 3. Agent 重连后重发 FirstConnect —— 复现 FrameBuilder 的注入: 顶层带旧 sid
	stale := jsonFrame(t, map[string]interface{}{
		"cmd":        CmdFirstConnect,
		"session_id": sid, // ← FrameBuilder::InjectSessionId 补的旧值
		"params":     map[string]interface{}{"machine_id": "restart-m2", "hostname": "h2"},
	})
	got, err := m.BeaconFrame("10.0.0.11:444", sid, stale)
	if err != nil {
		t.Fatalf("BeaconFrame: %v", err)
	}

	// 4. 必须完成注册确认(Agent 靠它进主循环), 且复用原 sid、不新增会话
	if n := len(m.sessions); n != 1 {
		t.Fatalf("重新注册不应新增会话, 会话数应为 1, got %d", n)
	}
	cf, ok := got.PollOutbox()
	if !ok {
		t.Fatal("重新注册必须回确认帧(否则 Agent 8s 超时 → 重连死循环)")
	}
	var resp map[string]interface{}
	if err := json.Unmarshal(cf, &resp); err != nil {
		t.Fatalf("确认帧解析失败: %v", err)
	}
	data, _ := resp["data"].(map[string]interface{})
	if data == nil {
		t.Fatalf("确认帧应含 data.session_id, got %v", resp)
	}
	if gotSid, _ := data["session_id"].(float64); int(gotSid) != sid {
		t.Fatalf("应按 machine_id 复用原会话 #%d, got %v", sid, gotSid)
	}
}

// TestBeaconUnknownSessionRefusesAnonymousRegistration 回归: sid 既不在内存、也无持久记录时,
// 绝不铸造匿名会话 —— 回 "请重新注册"(SESSION_UNKNOWN), 由 Agent 清空 sid 后重发 FirstConnect 自愈。
func TestBeaconUnknownSessionRefusesAnonymousRegistration(t *testing.T) {
	m := newTestManager()

	got, err := m.BeaconFrame("10.0.0.9:444", 424242,
		jsonFrame(t, map[string]interface{}{"cmd": CmdPoll, "session_id": 424242}))
	if err != nil {
		t.Fatalf("BeaconFrame: %v", err)
	}
	if n := len(m.sessions); n != 0 {
		t.Fatalf("未知 sid 不得铸造会话, 会话数应为 0, got %d", n)
	}
	assertReregisterSignal(t, got)
}

// TestBeaconNonRegistrationFirstFrameDoesNotMint 回归: 首帧为非注册命令(无 sid)同样不得铸造 ——
// 这是 #99→#113 与"重启风暴"共用的第三个铸造入口(ResolveIdentity 的 default 分支)。
func TestBeaconNonRegistrationFirstFrameDoesNotMint(t *testing.T) {
	m := newTestManager()

	next, err := m.HandleBeaconFrame(m.NewBeaconSession("10.0.0.10:444"),
		jsonFrame(t, map[string]interface{}{"cmd": CmdPoll}))
	if err != nil {
		t.Fatalf("HandleBeaconFrame: %v", err)
	}
	if n := len(m.sessions); n != 0 {
		t.Fatalf("非注册首帧不得铸造会话, 会话数应为 0, got %d", n)
	}
	assertReregisterSignal(t, next)
}
