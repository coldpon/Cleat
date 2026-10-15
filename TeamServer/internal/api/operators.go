package api

import (
	"encoding/json"
	"net/http"

	"cleat/teamserver/internal/apierr"
	"cleat/teamserver/internal/auth"
)

// GET /api/operators 操作员列表(admin/operator 可读; viewer 不可见敏感令牌)。
func (s *Server) listOperators(w http.ResponseWriter, r *http.Request) {
	identity, _ := auth.FromContext(r.Context())
	if !identity.IsAdmin() && !identity.CanWrite() {
		writeErr(w, http.StatusForbidden, apierr.CodeDenied, "无权限查看操作员")
		return
	}
	writeJSON(w, http.StatusOK, map[string]interface{}{"operators": s.svc.OperatorList()})
}

// POST /api/operators 新增/更新操作员(仅 admin)。
// body: {"username","role","token","active"?}
func (s *Server) addOperator(w http.ResponseWriter, r *http.Request) {
	identity, _ := auth.FromContext(r.Context())
	if !identity.IsAdmin() {
		writeErr(w, http.StatusForbidden, apierr.CodeDenied, "仅 admin 可管理操作员")
		return
	}
	var req struct {
		Username string `json:"username"`
		Role     string `json:"role"`
		Token    string `json:"token"`
		Active   *bool  `json:"active"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeErr(w, http.StatusBadRequest, apierr.CodeInvalidParam, "无效 JSON: "+err.Error())
		return
	}
	active := true
	if req.Active != nil {
		active = *req.Active
	}
	if err := s.svc.OperatorAdd(identity.Username, req.Username, req.Role, req.Token, active); err != nil {
		serviceErr(w, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]string{"status": "ok"})
}

// DELETE /api/operators/{name} 删除操作员(仅 admin)。
func (s *Server) removeOperator(w http.ResponseWriter, r *http.Request) {
	identity, _ := auth.FromContext(r.Context())
	if !identity.IsAdmin() {
		writeErr(w, http.StatusForbidden, apierr.CodeDenied, "仅 admin 可管理操作员")
		return
	}
	name := r.PathValue("name")
	if name == "" {
		writeErr(w, http.StatusBadRequest, apierr.CodeInvalidParam, "缺少操作员名")
		return
	}
	s.svc.OperatorRemove(identity.Username, name)
	writeJSON(w, http.StatusOK, map[string]string{"status": "ok"})
}

// POST /api/operators/{name}/password 修改操作员密码。
// admin 可改任何人; operator/viewer 只能改自己。
func (s *Server) changePassword(w http.ResponseWriter, r *http.Request) {
	identity, _ := auth.FromContext(r.Context())
	name := r.PathValue("name")
	if name == "" {
		writeErr(w, http.StatusBadRequest, apierr.CodeInvalidParam, "缺少操作员名")
		return
	}
	// 权限: admin 可改任何人; 其他角色只能改自己
	if !identity.IsAdmin() && identity.Username != name {
		writeErr(w, http.StatusForbidden, apierr.CodeDenied, "只能修改自己的密码")
		return
	}
	var req struct {
		OldPassword string `json:"old_password"`
		NewPassword string `json:"new_password"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeErr(w, http.StatusBadRequest, apierr.CodeInvalidParam, "无效 JSON")
		return
	}
	if err := s.svc.ChangePassword(identity.Username, name, req.OldPassword, req.NewPassword, identity.IsAdmin()); err != nil {
		serviceErr(w, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]string{"status": "ok"})
}

// POST /api/operators/{name}/toggle 启用/禁用操作员(仅 admin)。
func (s *Server) toggleOperator(w http.ResponseWriter, r *http.Request) {
	identity, _ := auth.FromContext(r.Context())
	if !identity.IsAdmin() {
		writeErr(w, http.StatusForbidden, apierr.CodeDenied, "仅 admin 可管理操作员")
		return
	}
	name := r.PathValue("name")
	if name == "" {
		writeErr(w, http.StatusBadRequest, apierr.CodeInvalidParam, "缺少操作员名")
		return
	}
	active, err := s.svc.OperatorToggle(identity.Username, name)
	if err != nil {
		serviceErr(w, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]interface{}{"status": "ok", "active": active})
}
