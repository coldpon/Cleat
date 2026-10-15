package api

import (
	"encoding/json"
	"errors"
	"net/http"

	"cleat/teamserver/internal/apierr"
	"cleat/teamserver/internal/auth"
	"cleat/teamserver/internal/service"
)

// GET /api/modules 列出所有模块。
func (s *Server) moduleList(w http.ResponseWriter, r *http.Request) {
	list, err := s.svc.ModuleList()
	if err != nil {
		serviceErr(w, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]interface{}{"modules": list})
}

// POST /api/modules?name=<module_id> 上传模块 DLL。
func (s *Server) moduleUpload(w http.ResponseWriter, r *http.Request) {
	identity, _ := auth.FromContext(r.Context())
	if !identity.CanWrite() {
		writeErr(w, http.StatusForbidden, apierr.CodeDenied, "viewer 无权限上传模块")
		return
	}
	name := r.URL.Query().Get("name")
	if name == "" {
		writeErr(w, http.StatusBadRequest, apierr.CodeInvalidParam, "缺少 name 参数")
		return
	}
	if err := s.svc.ValidateModuleName(name); err != nil {
		writeErr(w, http.StatusBadRequest, apierr.CodeInvalidParam, err.Error())
		return
	}
	info, err := s.svc.ModuleUpload(identity.Username, name, r.Body)
	if err != nil {
		if errors.Is(err, service.ErrModuleExists) {
			writeErr(w, http.StatusConflict, apierr.CodeAlreadyExists, err.Error())
			return
		}
		writeErr(w, http.StatusBadRequest, apierr.CodeInvalidParam, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, map[string]interface{}{
		"name": info.Name, "size": info.Size, "stored": true,
	})
}

// DELETE /api/modules/{name} 删除模块。
func (s *Server) moduleDelete(w http.ResponseWriter, r *http.Request) {
	identity, _ := auth.FromContext(r.Context())
	if !identity.CanWrite() {
		writeErr(w, http.StatusForbidden, apierr.CodeDenied, "viewer 无权限删除模块")
		return
	}
	name := r.PathValue("name")
	if err := s.svc.ModuleDelete(identity.Username, name); err != nil {
		if errors.Is(err, service.ErrModuleNotFound) {
			writeErr(w, http.StatusNotFound, apierr.CodeNotFound, err.Error())
			return
		}
		serviceErr(w, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]interface{}{"deleted": true})
}

// POST /api/sessions/{id}/module-load 按名下发模块到会话。
func (s *Server) moduleLoad(w http.ResponseWriter, r *http.Request) {
	identity, _ := auth.FromContext(r.Context())
	if !identity.CanWrite() {
		writeErr(w, http.StatusForbidden, apierr.CodeDenied, "viewer 无权限下发模块")
		return
	}
	id := pathInt(r, "id")
	if id <= 0 {
		writeErr(w, http.StatusBadRequest, apierr.CodeInvalidParam, "无效会话 ID")
		return
	}
	var req struct {
		ModuleID string `json:"module_id"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil || req.ModuleID == "" {
		writeErr(w, http.StatusBadRequest, apierr.CodeInvalidParam, "缺少 module_id")
		return
	}
	resp, err := s.svc.ModuleLoad(identity.Username, id, req.ModuleID)
	if err != nil {
		if errors.Is(err, service.ErrModuleNotFound) {
			writeErr(w, http.StatusNotFound, apierr.CodeNotFound, err.Error())
			return
		}
		serviceErr(w, err)
		return
	}
	writeJSON(w, http.StatusOK, resp)
}
