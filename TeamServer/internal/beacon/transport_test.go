package beacon

// F1(复核后小尾巴, ): beacon 通道类型按连接 TLS 状态定性 ——
// 明文 HTTP 记 "http", TLS 记 "https"。修复前 http.go 硬编码 "https",
// 开发态纯 HTTP beacon 会话会被误记为 "https"(与"通道由 listener 定性"不符)。

import (
	"bytes"
	"crypto/tls"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"cleat/teamserver/internal/crypto"
	"cleat/teamserver/internal/eventbus"
	"cleat/teamserver/internal/session"
	"cleat/teamserver/internal/store"
)

// transportByHostname 返回指定 hostname 会话的 transport。
func transportByHostname(t *testing.T, mgr *session.Manager, hostname string) string {
	t.Helper()
	for _, s := range mgr.SessionInfos() {
		if s.Hostname == hostname {
			return s.Transport
		}
	}
	t.Fatalf("未找到 hostname=%q 的会话", hostname)
	return ""
}

// TestHandlerTransportByTLS 端到端: 同一 Upload handler, 明文 HTTP → "http", TLS → "https"。
func TestHandlerTransportByTLS(t *testing.T) {
	cbox, err := crypto.New(strings.Repeat("ab", 32))
	if err != nil {
		t.Fatalf("crypto.New: %v", err)
	}
	mgr := session.NewManager(store.New(), eventbus.New(), cbox, nil, false)
	upload := New(mgr, cbox, nil).Upload()

	register := func(machineID string, tlsState *tls.ConnectionState) {
		fc := fmt.Sprintf(`{"cmd":"FirstConnect","ts":%d,"params":{"machine_id":%q,"hostname":%q}}`,
			time.Now().Unix(), machineID, machineID)
		enc, err := cbox.Encrypt([]byte(fc))
		if err != nil {
			t.Fatalf("Encrypt(%s): %v", machineID, err)
		}
		req := httptest.NewRequest("POST", "/upload", bytes.NewReader(enc))
		req.TLS = tlsState // nil = 明文 HTTP; 非 nil = HTTPS
		rec := httptest.NewRecorder()
		upload(rec, req)
		if rec.Code != http.StatusOK {
			t.Fatalf("%s 注册应 200, got %d", machineID, rec.Code)
		}
	}

	// 明文 HTTP: r.TLS == nil → 通道 "http"(修复前会被误记为 "https")
	register("plain-m1", nil)
	if got := transportByHostname(t, mgr, "plain-m1"); got != "http" {
		t.Fatalf("明文 HTTP 会话 transport 应为 http, got %q", got)
	}

	// TLS: r.TLS != nil → 通道 "https"
	register("tls-m1", &tls.ConnectionState{})
	if got := transportByHostname(t, mgr, "tls-m1"); got != "https" {
		t.Fatalf("TLS 会话 transport 应为 https, got %q", got)
	}
}
