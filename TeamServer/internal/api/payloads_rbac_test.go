package api

//   - 写操作(generate/delete) 与读操作(stubs/list/download) 一律 CanWrite;
//   - viewer → 403 Denied; operator/admin 读 → 200; 无 token → 非 200。
// 复用 targets_perm_test.go 的 newPermTestServer 播种三视角。

import (
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"cleat/teamserver/internal/apierr"
)

// doMethod 带方法/体发起请求(GET 之外用)。
func doMethod(t *testing.T, method, url, token, body string) *http.Response {
	t.Helper()
	var rd io.Reader
	if body != "" {
		rd = strings.NewReader(body)
	}
	req, err := http.NewRequest(method, url, rd)
	if err != nil {
		t.Fatalf("NewRequest: %v", err)
	}
	if token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatalf("Do: %v", err)
	}
	return resp
}

func TestPayloadsRBAC(t *testing.T) {
	s, _ := newPermTestServer(t)
	srv := httptest.NewServer(s.Handler())
	defer srv.Close()

	// viewer 对全部端点(generate/delete + stubs/list/download) → 403 Denied
	viewerCases := []struct{ method, path, body string }{
		{http.MethodPost, "/api/payloads/generate", `{"transport":"tcp","host":"h","port":"1"}`},
		{http.MethodDelete, "/api/payloads/whatever", ""},
		{http.MethodGet, "/api/payloads/stubs", ""},
		{http.MethodGet, "/api/payloads", ""},
		{http.MethodGet, "/api/payloads/whatever/download", ""},
	}
	for _, tc := range viewerCases {
		resp := doMethod(t, tc.method, srv.URL+tc.path, "tok-view", tc.body)
		if resp.StatusCode != http.StatusForbidden {
			_ = resp.Body.Close()
			t.Fatalf("%s %s viewer 应 403, got %d", tc.method, tc.path, resp.StatusCode)
		}
		p := decodeProblem(t, resp)
		_ = resp.Body.Close()
		if p.Code != apierr.CodeDenied || p.Domain != apierr.DomainApi {
			t.Fatalf("%s %s code/domain = %s/%s, want %s/API", tc.method, tc.path, p.Code, p.Domain, apierr.CodeDenied)
		}
	}

	// operator/admin 读 → 200
	for _, tok := range []string{"tok-oper", "tok-admin"} {
		for _, path := range []string{"/api/payloads/stubs", "/api/payloads"} {
			resp := doMethod(t, http.MethodGet, srv.URL+path, tok, "")
			code := resp.StatusCode
			_ = resp.Body.Close()
			if code != http.StatusOK {
				t.Fatalf("%s GET %s 应 200, got %d", tok, path, code)
			}
		}
	}

	// operator 下载不存在的样本: 守卫已过 → 404(而非 403), 证明读守卫对写角色放行
	resp := doMethod(t, http.MethodGet, srv.URL+"/api/payloads/no-such-payload/download", "tok-oper", "")
	code := resp.StatusCode
	_ = resp.Body.Close()
	if code != http.StatusNotFound {
		t.Fatalf("operator 下载不存在产物应 404, got %d", code)
	}

	// 未认证 → 非 200(401)
	resp = doMethod(t, http.MethodGet, srv.URL+"/api/payloads/stubs", "", "")
	code = resp.StatusCode
	_ = resp.Body.Close()
	if code == http.StatusOK {
		t.Fatalf("无 token 不应放行, got %d", code)
	}
}
