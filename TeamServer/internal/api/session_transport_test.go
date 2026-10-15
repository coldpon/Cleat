package api

// 会话列表 transport 字段端到端校验(Web 修复批 1-A, ):
// 非 TCP 通道建立会话后, GET /api/sessions 的 JSON 必须带真实 transport 值。
// 这是 Web 会话列表"通道"列(修复前硬编码 'tcp')的服务端对账点。

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	"cleat/teamserver/internal/eventbus"
	"cleat/teamserver/internal/service"
	"cleat/teamserver/internal/session"
	"cleat/teamserver/internal/store"
)

// TestSessionsListExposesTransport 经 DNS 通道注册会话 → 列表接口应回 transport=dns(非 'tcp')。
func TestSessionsListExposesTransport(t *testing.T) {
	st := store.New()
	if err := st.SaveOperator(&store.Operator{Username: "admin", Role: store.RoleAdmin, Token: "tok-admin", Active: true}); err != nil {
		t.Fatalf("SaveOperator: %v", err)
	}
	bus := eventbus.New()
	mgr := session.NewManager(st, bus, nil, nil, false)
	svc := service.New(mgr, st, nil, "tok-admin")
	s := New(svc, bus, "tok-admin")

	// 经 beacon 请求驱动通道(DNS)注册一个会话 —— 非 TCP。
	body, _ := json.Marshal(map[string]interface{}{
		"cmd": session.CmdFirstConnect,
		"ts":  float64(time.Now().Unix()),
		"params": map[string]interface{}{
			"machine_id": "api-m1", "hostname": "api-host",
		},
	})
	sess, err := mgr.BeaconFrame("10.1.1.1:5", 0, body, "dns")
	if err != nil || sess == nil {
		t.Fatalf("BeaconFrame: sess=%v err=%v", sess, err)
	}

	srv := httptest.NewServer(s.Handler())
	defer srv.Close()

	req, _ := http.NewRequest("GET", srv.URL+"/api/sessions", nil)
	req.Header.Set("Authorization", "Bearer tok-admin")
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatalf("GET /api/sessions: %v", err)
	}
	defer func() { _ = resp.Body.Close() }()
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("状态码应 200, got %d", resp.StatusCode)
	}

	var out struct {
		Sessions []map[string]interface{} `json:"sessions"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&out); err != nil {
		t.Fatalf("decode: %v", err)
	}
	if len(out.Sessions) != 1 {
		t.Fatalf("应有 1 条会话, got %d", len(out.Sessions))
	}
	if got := out.Sessions[0]["transport"]; got != "dns" {
		t.Fatalf("列表 transport 应为 dns, got %v", got)
	}
}
