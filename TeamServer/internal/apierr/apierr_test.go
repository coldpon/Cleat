package apierr

import (
	"errors"
	"fmt"
	"net/http"
	"testing"
)

// 生成件一致性: class 映射必须与全量码表一一对应(防 catalog 改后忘 regen)。
func TestClassMapCoversAllCodes(t *testing.T) {
	if len(codeClass) != len(AllCodes) {
		t.Fatalf("codeClass %d 项 != AllCodes %d 项(生成件不一致?)", len(codeClass), len(AllCodes))
	}
	for _, c := range AllCodes {
		if c.Class() == "" {
			t.Errorf("Code %q 缺 class", c)
		}
	}
}

// HTTPStatus 必须对每个已定义语义给出状态码; 未知一律 500。
func TestHTTPStatusTotal(t *testing.T) {
	for _, c := range AllCodes {
		if HTTPStatus(c) == 0 {
			t.Errorf("Code %q 无 HTTP 状态映射", c)
		}
	}
	if got := HTTPStatus(CodeUnknown); got != http.StatusInternalServerError {
		t.Errorf("Unknown 兜底应为 500, got %d", got)
	}
	if got := HTTPStatus(Code("不存在的码")); got != http.StatusInternalServerError {
		t.Errorf("未知名应为 500, got %d", got)
	}
}

// errors.Is 只按 Code 判定, 域不参与; 包装链可穿透。
func TestErrorsIsByCodeOnly(t *testing.T) {
	base := New(DomainApi, CodeNotFound, "task not found")
	wrapped := fmt.Errorf("handle submit: %w", base)

	if !errors.Is(wrapped, &Error{Code: CodeNotFound}) {
		t.Fatal("同码应命中(errors.Is 穿透 fmt 包装)")
	}
	if errors.Is(wrapped, &Error{Code: CodeDenied}) {
		t.Fatal("异码不应命中")
	}
	if !errors.Is(base, &Error{Code: CodeNotFound, Domain: DomainTs}) {
		t.Fatal("域不参与判定: 同码异域应命中")
	}
}

// Wrap/From 保留并提取 cause 链。
func TestFromAndCauseChain(t *testing.T) {
	cause := errors.New("eof")
	base := Wrap(DomainNet, CodePeerDisconnected, "peer fin", cause)
	outer := fmt.Errorf("recv loop: %w", base)

	got := From(outer)
	if got == nil || got.Code != CodePeerDisconnected || got.Domain != DomainNet {
		t.Fatalf("From 提取失败: %+v", got)
	}
	if !errors.Is(outer, cause) {
		t.Fatal("cause 链应可穿透")
	}
	if From(errors.New("plain")) != nil {
		t.Fatal("无码错误 From 应返回 nil")
	}
}

// 日志渲染格式与 Agent 侧 CLEAT_LOGE 一致; 空消息兜底为码名。
func TestLogFieldsFormat(t *testing.T) {
	e := New(DomainApi, CodeNotFound, "task not found")
	if got := e.LogFields(); got != "code=NotFound domain=API | task not found" {
		t.Fatalf("日志渲染格式漂移: %q", got)
	}
	if got := New(DomainApi, CodeUnknown, "").LogFields(); got != "code=Unknown domain=API | Unknown" {
		t.Fatalf("空消息兜底: %q", got)
	}
}

// Problem(RFC 9457)字段组装: type 由 slug 派生, title 来自 catalog, status 与响应一致。
func TestProblemShape(t *testing.T) {
	p := NewProblem(404, DomainApi, CodeNotFound, "task not found")
	if p.Type != ProblemTypeBase+"not-found" {
		t.Fatalf("type = %q", p.Type)
	}
	if p.Title != "目标不存在" {
		t.Fatalf("title = %q(应为 catalog title)", p.Title)
	}
	if p.Status != 404 || p.Code != CodeNotFound || p.Domain != DomainApi || p.Detail != "task not found" {
		t.Fatalf("problem 字段错位: %+v", p)
	}
	// 未知码: 不猜文案, slug 兜底 "unknown"
	if Code("NoSuchCode").Title() != "NoSuchCode" || Code("NoSuchCode").Slug() != "unknown" {
		t.Fatal("未知码应回退码名/unknown, 不得猜")
	}
}

// 认证/冲突语义的 HTTP 映射。
func TestHTTPStatusAuthAndConflict(t *testing.T) {
	if got := HTTPStatus(CodeUnauthenticated); got != http.StatusUnauthorized {
		t.Fatalf("Unauthenticated → %d, want 401", got)
	}
	if got := HTTPStatus(CodeAlreadyExists); got != http.StatusConflict {
		t.Fatalf("AlreadyExists → %d, want 409", got)
	}
}
