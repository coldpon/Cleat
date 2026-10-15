// Package apierr 提供 Cleat 统一错误码(二维模型: 语义×域)的 Go 侧载体。
//
// 码表是生成件(codes_gen.go), 唯一事实源 doc/errors/catalog.toml。
// 约定:
//   - Code(语义) 是稳定契约, 永不复用/改名; Domain(域) 是统计归属, 不参与判定。
//   - 内部错误保持 Go 原生(sentinel + %w); 只有跨边界(HTTP/帧)时才需要携带码 ——
//     收敛点未就位前, 不要为"顺手"给每条内部错误套码。
//   - 判定用 errors.Is(按 Code); 禁止对 err.Error() 做字符串匹配(与 Agent 侧同规)。
//
// 用法:
//
//	return apierr.New(apierr.DomainApi, apierr.CodeNotFound, "task not found")
//	return apierr.Wrap(apierr.DomainApi, apierr.CodeInternalFailed, "load config", err)
package apierr

import (
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
)

// ProblemTypeBase 是 RFC 9457 type URI 的基址。
// **未定稿**: 域名落定后统一替换(单一常量, 只改这一处)。
const ProblemTypeBase = "https://cleat.dev/probs/"

// Problem 是 RFC 9457(application/problem+json)响应体。
// code/domain 为规范明文允许的扩展成员 —— 三端 catalog 的精确串;
// 前端优先按 code 查 i18n, title/detail 只作兜底展示。
type Problem struct {
	Type   string `json:"type"`             // 问题类型 URI(机器标识, 由 code slug 派生)
	Title  string `json:"title"`            // 类型级人读标题(catalog title)
	Status int    `json:"status"`           // HTTP 状态码(与响应行一致)
	Detail string `json:"detail,omitempty"` // 本次实例的具体说明(可含上下文)
	Code   Code   `json:"code"`             // 语义码(精确, 前端按此分支)
	Domain Domain `json:"domain"`           // 域(统计归属)
}

// NewProblem 组装 problem 体。
func NewProblem(status int, dom Domain, code Code, detail string) Problem {
	return Problem{
		Type:   ProblemTypeBase + code.Slug(),
		Title:  code.Title(),
		Status: status,
		Detail: detail,
		Code:   code,
		Domain: dom,
	}
}

// WriteProblem 写出 problem+json 错误响应(API 边界唯一出口, 见 api.writeErr/serviceErr)。
func WriteProblem(w http.ResponseWriter, status int, dom Domain, code Code, detail string) {
	w.Header().Set("Content-Type", "application/problem+json; charset=utf-8")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(NewProblem(status, dom, code, detail))
}

// Error 带错误码的错误, 实现 error / Unwrap / Is。
type Error struct {
	Domain Domain // 统计归属(谁报的)
	Code   Code   // 语义(为什么错) —— errors.Is 只按此判定
	Msg    string // 人读消息(不含码; 码由渲染层拼, 见 LogFields)
	cause  error  // 底层原因(可空)
}

// New 构造带码错误。
func New(dom Domain, code Code, msg string) *Error {
	return &Error{Domain: dom, Code: code, Msg: msg}
}

// Wrap 包装底层错误并携带码(保留 cause 链, 供 errors.Is/As 穿透)。
func Wrap(dom Domain, code Code, msg string, cause error) *Error {
	return &Error{Domain: dom, Code: code, Msg: msg, cause: cause}
}

// Error 实现 error。消息本身不含码 —— 与 Agent 侧 CLEAT_LOGE 的字段化渲染一致,
// 避免码在 message 里重复出现污染人读文本。
func (e *Error) Error() string {
	if e.Msg != "" {
		return e.Msg
	}
	return string(e.Code)
}

// Unwrap 供 errors.Is/As 穿透到 cause 链。
func (e *Error) Unwrap() error { return e.cause }

// Is 判定语义: 目标为 *Error 时仅比较 Code。
// 域是统计归属, 故意不参与判定 —— 同一语义从不同域报出应能被同一个 Is 捕获。
func (e *Error) Is(target error) bool {
	t, ok := target.(*Error)
	if !ok {
		return false
	}
	return e.Code == t.Code
}

// From 提取错误链上第一个 *Error; 无则返回 nil。
func From(err error) *Error {
	var e *Error
	if errors.As(err, &e) {
		return e
	}
	return nil
}

// LogFields 渲染与 Agent 侧 CLEAT_LOGE 完全一致的日志字段串:
//
//	code=<语义> domain=<域> | <消息>
//
// 日志层(C2 批次接入 logx)只此一处拼格式, 防止三端渲染漂移。
func (e *Error) LogFields() string {
	return fmt.Sprintf("code=%s domain=%s | %s", e.Code, e.Domain, e.Error())
}

// HTTPStatus 语义 → HTTP 状态码(API 边界使用)。
// 未列出的语义一律 500 —— 未分类错误不得伪装成 4xx(与既有 statusFromServiceErr 缺省一致)。
func HTTPStatus(c Code) int {
	switch c {
	case CodeInvalidParam:
		return http.StatusBadRequest
	case CodeUnauthenticated:
		return http.StatusUnauthorized
	case CodeDenied:
		return http.StatusForbidden
	case CodeNotFound:
		return http.StatusNotFound
	case CodeAlreadyExists:
		return http.StatusConflict
	case CodeResourceExhausted:
		return http.StatusTooManyRequests
	case CodeNotSupported:
		return http.StatusNotImplemented
	default:
		return http.StatusInternalServerError
	}
}
