package api

// 错误响应契约测试: 验证 RFC 9457 problem+json 的真实 HTTP 形态 ——
// Content-Type / 标准字段 / 扩展码域, 三条代表性路径(401/400/404)。
// 这是三端契约的服务端一侧锚点: 前端 requests.ts 的解析与此处逐字段对应。

import (
	"bytes"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"cleat/teamserver/internal/apierr"
)

func decodeProblem(t *testing.T, resp *http.Response) apierr.Problem {
	t.Helper()
	ct := resp.Header.Get("Content-Type")
	if !strings.Contains(ct, "application/problem+json") {
		t.Fatalf("Content-Type = %q, want problem+json", ct)
	}
	var p apierr.Problem
	if err := json.NewDecoder(resp.Body).Decode(&p); err != nil {
		t.Fatalf("decode problem: %v", err)
	}
	if p.Status != resp.StatusCode {
		t.Fatalf("body.status=%d != http status=%d(RFC 9457 要求一致)", p.Status, resp.StatusCode)
	}
	return p
}

// 401: 未携带 token → Unauthenticated。
func TestProblemUnauthenticated(t *testing.T) {
	s, _, _ := newTestServerWithModule(t)
	srv := httptest.NewServer(s.Handler())
	defer srv.Close()

	resp, err := http.Get(srv.URL + "/api/sessions")
	if err != nil {
		t.Fatalf("Get: %v", err)
	}
	defer func() { _ = resp.Body.Close() }()

	if resp.StatusCode != http.StatusUnauthorized {
		t.Fatalf("status = %d, want 401", resp.StatusCode)
	}
	p := decodeProblem(t, resp)
	if p.Code != apierr.CodeUnauthenticated || p.Domain != apierr.DomainApi {
		t.Fatalf("code/domain = %s/%s, want Unauthenticated/API", p.Code, p.Domain)
	}
	if p.Type != apierr.ProblemTypeBase+"unauthenticated" {
		t.Fatalf("type = %q", p.Type)
	}
}

// 400: task id 解析失败 → InvalidParam(状态沿用旧行为 400)。
func TestProblemInvalidParam(t *testing.T) {
	s, _, _ := newTestServerWithModule(t)
	srv := httptest.NewServer(s.Handler())
	defer srv.Close()

	req, _ := http.NewRequest("GET", srv.URL+"/api/tasks/abc", nil)
	req.Header.Set("Authorization", "Bearer tok-admin")
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatalf("Do: %v", err)
	}
	defer func() { _ = resp.Body.Close() }()

	if resp.StatusCode != http.StatusBadRequest {
		t.Fatalf("status = %d, want 400", resp.StatusCode)
	}
	p := decodeProblem(t, resp)
	if p.Code != apierr.CodeInvalidParam {
		t.Fatalf("code = %s, want InvalidParam", p.Code)
	}
	if p.Detail == "" {
		t.Fatal("detail 不应为空")
	}
}

// 404: 任务不存在 → NotFound。
func TestProblemNotFound(t *testing.T) {
	s, _, _ := newTestServerWithModule(t)
	srv := httptest.NewServer(s.Handler())
	defer srv.Close()

	req, _ := http.NewRequest("GET", srv.URL+"/api/tasks/999", nil)
	req.Header.Set("Authorization", "Bearer tok-admin")
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatalf("Do: %v", err)
	}
	defer func() { _ = resp.Body.Close() }()

	if resp.StatusCode != http.StatusNotFound {
		t.Fatalf("status = %d, want 404", resp.StatusCode)
	}
	p := decodeProblem(t, resp)
	if p.Code != apierr.CodeNotFound {
		t.Fatalf("code = %s, want NotFound", p.Code)
	}
}

// 登录失败 → 401 Unauthenticated(公开端点, 不走 auth 中间件)。
func TestProblemLoginFailed(t *testing.T) {
	s, _, _ := newTestServerWithModule(t)
	srv := httptest.NewServer(s.Handler())
	defer srv.Close()

	body, _ := json.Marshal(map[string]string{"username": "nobody", "password": "wrong"})
	resp, err := http.Post(srv.URL+"/api/login", "application/json", bytes.NewReader(body))
	if err != nil {
		t.Fatalf("Post: %v", err)
	}
	defer func() { _ = resp.Body.Close() }()

	if resp.StatusCode != http.StatusUnauthorized {
		t.Fatalf("status = %d, want 401", resp.StatusCode)
	}
	p := decodeProblem(t, resp)
	if p.Code != apierr.CodeUnauthenticated {
		t.Fatalf("code = %s, want Unauthenticated", p.Code)
	}
}

// TS-2: 500 类错误收敛 —— serviceErr 兜底分支走 writeInternal, detail 固定文案,
// 原始 err(可能含 SQLite/绝对路径/Go 内部串)只进日志不出 HTTP 边界。
func TestServiceErrInternalSanitized(t *testing.T) {
	w := httptest.NewRecorder()
	// 未分类错误(非 *service.Error / 非 sentinel) → 兜底 500; 人为植入敏感串验证不外泄
	serviceErr(w, fmt.Errorf("pq: 拒绝连接 sqlite 队列满 (file /tmp/x.db)"))
	p := decodeProblem(t, w.Result())
	if p.Status != http.StatusInternalServerError || p.Code != apierr.CodeInternalFailed {
		t.Fatalf("status/code = %d/%s, want 500/InternalFailed", p.Status, p.Code)
	}
	if p.Detail != "内部错误" {
		t.Fatalf("detail = %q, 应为固定文案(不外泄 err.Error())", p.Detail)
	}
	if strings.ContainsAny(p.Detail, "/") || strings.Contains(p.Detail, "sqlite") {
		t.Fatalf("detail 疑似泄露内部细节: %q", p.Detail)
	}
}
