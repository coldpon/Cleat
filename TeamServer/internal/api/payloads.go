//
// 路由(紧邻监听器路由块之后, 见 api.go):
//
//	GET    /api/payloads/stubs            可用 stub(含 agent_commit 陈旧提示)
//	POST   /api/payloads/generate         烧录生成样本(零工具链)
//	GET    /api/payloads                  已生成样本清单
//	GET    /api/payloads/{name}/download  下载样本(白名单防穿越)
//	DELETE /api/payloads/{name}           删除样本
//
// 授权(对齐 listeners.go): **读操作亦收敛为 CanWrite** ——
// ①与 F5 的读旁路收口原则一致; ②下载含 server_pubkey + recovery.key 密钥材料, 绝不可给只读角色;
// ③前端 MENU_ROLES/RequireRole 已完全隐藏该页。全部走全局 auth 中间件(api.go)。
// 业务一律经 service 层; 本文件只做参数转换/权限判断/呈现。
package api

import (
	"encoding/json"
	"net/http"

	"cleat/teamserver/internal/apierr"
	"cleat/teamserver/internal/auth"
	"cleat/teamserver/internal/service"
)

// GET /api/payloads/stubs 返回内置/外部 stub 元数据列表。viewer 拒绝(见文件头授权说明)。
func (s *Server) listPayloadStubs(w http.ResponseWriter, r *http.Request) {
	identity, _ := auth.FromContext(r.Context())
	if !identity.CanWrite() {
		writeErr(w, http.StatusForbidden, apierr.CodeDenied, "viewer 无权限查看样本生成器")
		return
	}
	writeJSON(w, http.StatusOK, map[string]interface{}{
		"stubs": s.svc.PayloadStubs(),
	})
}

// POST /api/payloads/generate 模板烧录生成样本(同步, 毫秒级)。
// body = service.PayloadSpec(见 烧录 JSON 契约)。
func (s *Server) generatePayload(w http.ResponseWriter, r *http.Request) {
	identity, _ := auth.FromContext(r.Context())
	if !identity.CanWrite() {
		writeErr(w, http.StatusForbidden, apierr.CodeDenied, "viewer 无权限生成样本")
		return
	}
	var spec service.PayloadSpec
	if err := json.NewDecoder(r.Body).Decode(&spec); err != nil {
		writeErr(w, http.StatusBadRequest, apierr.CodeInvalidParam, "无效 JSON")
		return
	}
	res, err := s.svc.GeneratePayload(identity.Username, &spec)
	if err != nil {
		serviceErr(w, err)
		return
	}
	writeJSON(w, http.StatusOK, res)
}

// GET /api/payloads 已生成样本清单(扫产物根)。viewer 拒绝。
func (s *Server) listPayloads(w http.ResponseWriter, r *http.Request) {
	identity, _ := auth.FromContext(r.Context())
	if !identity.CanWrite() {
		writeErr(w, http.StatusForbidden, apierr.CodeDenied, "viewer 无权限查看样本清单")
		return
	}
	ents, err := s.svc.ListPayloads()
	if err != nil {
		serviceErr(w, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]interface{}{
		"payloads": ents,
	})
}

// GET /api/payloads/{name}/download 下载样本二进制。viewer 拒绝(含密钥材料)。
// name 由 service.PayloadFile 白名单校验(basename, 拒绝目录穿越)。
func (s *Server) downloadPayload(w http.ResponseWriter, r *http.Request) {
	identity, _ := auth.FromContext(r.Context())
	if !identity.CanWrite() {
		writeErr(w, http.StatusForbidden, apierr.CodeDenied, "viewer 无权限下载样本")
		return
	}
	path, filename, err := s.svc.PayloadFile(r.PathValue("name"))
	if err != nil {
		serviceErr(w, err)
		return
	}
	// 下载文件名已白名单(无路径分隔符), 直接内联即可。
	w.Header().Set("Content-Disposition", "attachment; filename=\""+filename+"\"")
	http.ServeFile(w, r, path)
}

// DELETE /api/payloads/{name} 删除样本目录。
func (s *Server) deletePayload(w http.ResponseWriter, r *http.Request) {
	identity, _ := auth.FromContext(r.Context())
	if !identity.CanWrite() {
		writeErr(w, http.StatusForbidden, apierr.CodeDenied, "viewer 无权限删除样本")
		return
	}
	if err := s.svc.DeletePayload(identity.Username, r.PathValue("name")); err != nil {
		serviceErr(w, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]interface{}{"ok": true})
}
