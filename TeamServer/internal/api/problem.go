package api

// 错误响应的唯一出口与收敛点(sliver rpcError 模式, 错误码 C2 批次 ):
//   - 内部错误保持 Go 原生(sentinel + %w); 只有跨 HTTP 边界时才携带码。
//   - 状态码原则: 一律保持旧行为(本批次零状态码变更); 码按语义标注。
//   - 响应体: RFC 9457 application/problem+json(见 apierr.Problem)。

import (
	"errors"
	"net/http"

	"cleat/teamserver/internal/apierr"
	"cleat/teamserver/internal/logx"
	"cleat/teamserver/internal/service"
)

// writeErr 统一错误出口: status 为既有行为, code 为 catalog 语义标注。
func writeErr(w http.ResponseWriter, status int, code apierr.Code, detail string) {
	apierr.WriteProblem(w, status, apierr.DomainApi, code, detail)
}

// writeInternal 内部错误出口(TS-2): 对外固定文案, 对内留全文 —— 细节只进日志,
// 不出 HTTP 边界(防 err.Error() 泄露 SQLite/绝对路径/Go 内部串)。5xx 一律走这里;
// 4xx(InvalidParam/NotFound/Denied/...)的业务可读原文仍走 writeErr。
func writeInternal(w http.ResponseWriter, code apierr.Code, publicMsg string, cause error) {
	if cause != nil {
		logx.Printf("[api] internal error: %v\n", cause)
	}
	writeErr(w, http.StatusInternalServerError, code, publicMsg)
}

// serviceErr 把 service/领域错误收敛为 problem 响应 —— 边界唯一收敛点。
// 映射: *service.Error 按 Class(invalid→400 / notFound→404 / auth→401 / 未分类→500);
// 领域哨兵(会话/模块)显式收敛; 其余兜底 500。
func serviceErr(w http.ResponseWriter, err error) {
	var se *service.Error
	if errors.As(err, &se) {
		switch se.Class {
		case service.ErrInvalid:
			writeErr(w, http.StatusBadRequest, apierr.CodeInvalidParam, err.Error())
		case service.ErrNotFound:
			writeErr(w, http.StatusNotFound, apierr.CodeNotFound, err.Error())
		case service.ErrAuth:
			writeErr(w, http.StatusUnauthorized, apierr.CodeUnauthenticated, err.Error())
		default:
			writeInternal(w, apierr.CodeInternalFailed, "内部错误", err)
		}
		return
	}
	switch {
	case errors.Is(err, service.ErrNoSession):
		writeErr(w, http.StatusNotFound, apierr.CodeNotFound, err.Error())
	case errors.Is(err, service.ErrModuleExists):
		writeErr(w, http.StatusConflict, apierr.CodeAlreadyExists, err.Error())
	case errors.Is(err, service.ErrModuleNotFound):
		writeErr(w, http.StatusNotFound, apierr.CodeNotFound, err.Error())
	default:
		writeInternal(w, apierr.CodeInternalFailed, "内部错误", err)
	}
}
