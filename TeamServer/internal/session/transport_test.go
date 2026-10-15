package session

// 会话通道类型(transport)传递测试(Web 修复批 1-A, ):
// 锁定 NewBeaconSession / BeaconFrame 的可选 transport 参数在会话建立时写入 Record,
// 并经 Info() 对外暴露 —— 这是 Web 会话列表"通道"列不再伪造 'tcp' 的服务端前提。

import (
	"encoding/json"
	"testing"
	"time"

	"cleat/teamserver/internal/eventbus"
	"cleat/teamserver/internal/store"
)

// TestTransportOrDefault 可变参数取首值语义: 省略/空串 → 默认; 有值 → 原样。
func TestTransportOrDefault(t *testing.T) {
	cases := []struct {
		in   []string
		def  string
		want string
	}{
		{nil, "tcp", "tcp"},
		{[]string{}, "tcp", "tcp"},
		{[]string{""}, "tcp", "tcp"},
		{[]string{"ws"}, "tcp", "ws"},
		{[]string{"dns"}, "https", "dns"},
	}
	for _, c := range cases {
		if got := transportOrDefault(c.in, c.def); got != c.want {
			t.Fatalf("transportOrDefault(%v, %q) = %q, want %q", c.in, c.def, got, c.want)
		}
	}
}

// TestNewBeaconSessionTransport beacon 会话默认 https, 显式传入则按传入值。
func TestNewBeaconSessionTransport(t *testing.T) {
	m := newTestManager()
	if got := m.NewBeaconSession("1.2.3.4:1").Info().Transport; got != "https" {
		t.Fatalf("无参 beacon 会话 transport 应为默认 https, got %q", got)
	}
	if got := m.NewBeaconSession("1.2.3.4:2", "dns").Info().Transport; got != "dns" {
		t.Fatalf("显式 dns 通道 transport 应为 dns, got %q", got)
	}
}

// TestBeaconFrameTransportPropagates 经 BeaconFrame 首帧注册后, 通道类型随会话落库并可由 Info 读出。
func TestBeaconFrameTransportPropagates(t *testing.T) {
	m := newTestManager()
	body, _ := json.Marshal(map[string]interface{}{
		"cmd": CmdFirstConnect,
		"ts":  float64(time.Now().Unix()),
		"params": map[string]interface{}{
			"machine_id": "transport-m1", "hostname": "transport-host",
		},
	})
	next, err := m.BeaconFrame("10.0.0.7:9", 0, body, "dns")
	if err != nil {
		t.Fatalf("BeaconFrame: %v", err)
	}
	if next == nil {
		t.Fatal("注册后应返回会话")
	}
	if got := next.Info().Transport; got != "dns" {
		t.Fatalf("注册会话 transport 应为 dns, got %q", got)
	}
	// 服务端 store 中的持久记录也应带上同一通道类型(Web 列表数据源)
	if rec := m.st.Get(next.ID()); rec == nil || rec.Transport != "dns" {
		t.Fatalf("store 记录应带 transport=dns, got %+v", rec)
	}
}

// TestBeaconGetTransportRevive GET 取件走"服务端重启后按记录复活"路径时, 通道以本次连接为准。
func TestBeaconGetTransportRevive(t *testing.T) {
	st := store.New()
	cbox, xbox := testCrypto()
	m1 := NewManager(st, eventbus.New(), cbox, xbox, false)
	// 先经 https 注册一个会话
	body, _ := json.Marshal(map[string]interface{}{
		"cmd": CmdFirstConnect,
		"ts":  float64(time.Now().Unix()),
		"params": map[string]interface{}{
			"machine_id": "get-m1", "hostname": "get-host",
		},
	})
	sess, err := m1.BeaconFrame("10.0.0.8:9", 0, body, "https")
	if err != nil || sess == nil {
		t.Fatalf("注册失败: sess=%v err=%v", sess, err)
	}
	// 模拟服务端重启: 新 Manager 共享同一 store, 内存会话表为空
	m2 := NewManager(st, eventbus.New(), cbox, xbox, false)
	// 改用 dns 通道取件 → 走 bindRevivedRecord 复活路径, 通道类型以本次连接为准
	_ = m2.BeaconGet("10.0.0.8:9", "get-m1", 0, "dns")
	if rec := st.ByMachine("get-m1"); rec == nil || rec.Transport != "dns" {
		t.Fatalf("GET 复活后通道应更新为 dns, got %+v", rec)
	}
}
