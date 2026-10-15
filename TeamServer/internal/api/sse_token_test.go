package api

// SSE 短 token 的作用域与回收:
//   - 限路径: 仅 *$"/events" 端点放行(原先对所有端点生效, 泄露面过大)
//   - 周期回收: PruneSSETokens 由 main.go sweeper 同频调用(原先永不回收 → 无界增长)

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"
)

// 换短 token: POST /api/sse-token(Bearer 认证) → {"token": "..."}。
func issueSSEToken(t *testing.T, srvURL, bearer string) string {
	t.Helper()
	req, err := http.NewRequest(http.MethodGet, srvURL+"/api/sse-token", nil)
	if err != nil {
		t.Fatalf("NewRequest: %v", err)
	}
	req.Header.Set("Authorization", "Bearer "+bearer)
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatalf("Do(sse-token): %v", err)
	}
	defer func() { _ = resp.Body.Close() }()
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("sse-token status = %d, want 200", resp.StatusCode)
	}
	var body struct {
		Token string `json:"token"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&body); err != nil {
		t.Fatalf("decode: %v", err)
	}
	if body.Token == "" {
		t.Fatal("token 为空")
	}
	return body.Token
}

// 短 token 仅放行 events 端点; 其它端点一律 401; 且一次性(第二次即失效)。
func TestSSEShortTokenScopedToEvents(t *testing.T) {
	s, _, _ := newTestServerWithModule(t)
	srv := httptest.NewServer(s.Handler())
	defer srv.Close()

	client := &http.Client{Timeout: 5 * time.Second}
	tok := issueSSEToken(t, srv.URL, "tok-admin")

	// 正向: 短 token 调 /api/events → 200(SSE 建流; 读到响应头即断言后关闭)
	resp, err := client.Get(srv.URL + "/api/events?token=" + tok)
	if err != nil {
		t.Fatalf("Get(events): %v", err)
	}
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("events status = %d, want 200(短 token 应放行 events)", resp.StatusCode)
	}
	_ = resp.Body.Close()

	// 负向1: 同一 token(已消耗)调 /api/sessions → 401(限路径 + 一次性, 双重)
	resp2, err := client.Get(srv.URL + "/api/sessions?token=" + tok)
	if err != nil {
		t.Fatalf("Get(sessions): %v", err)
	}
	_ = resp2.Body.Close()
	if resp2.StatusCode != http.StatusUnauthorized {
		t.Fatalf("sessions status = %d, want 401(短 token 不放行非 events 端点)", resp2.StatusCode)
	}

	// 负向2: 换一个新 token 调 /api/sessions → 仍 401(证明是限路径而非消耗顺序)
	tok2 := issueSSEToken(t, srv.URL, "tok-admin")
	resp3, err := client.Get(srv.URL + "/api/sessions?token=" + tok2)
	if err != nil {
		t.Fatalf("Get(sessions): %v", err)
	}
	_ = resp3.Body.Close()
	if resp3.StatusCode != http.StatusUnauthorized {
		t.Fatalf("sessions status = %d, want 401(新 token 也应被限路径拒绝)", resp3.StatusCode)
	}
}

// PruneSSETokens 过期回收: 删过期、留未过期。
func TestPruneSSETokens(t *testing.T) {
	sseTokMu.Lock()
	sseTokStore = map[string]sseTokEntry{
		"expired": {expiry: time.Now().Add(-time.Minute)},
		"live":    {expiry: time.Now().Add(time.Minute)},
	}
	sseTokMu.Unlock()

	PruneSSETokens(time.Now())

	sseTokMu.Lock()
	_, hasExpired := sseTokStore["expired"]
	_, hasLive := sseTokStore["live"]
	sseTokMu.Unlock()
	if hasExpired {
		t.Fatal("过期 token 应被回收")
	}
	if !hasLive {
		t.Fatal("未过期 token 不应被回收")
	}
}
