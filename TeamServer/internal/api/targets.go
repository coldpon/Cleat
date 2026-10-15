package api

import (
	"encoding/json"
	"net/http"
	"strconv"

	"cleat/teamserver/internal/apierr"
	"cleat/teamserver/internal/auth"
	"cleat/teamserver/internal/service"
)

func (s *Server) listTargets(w http.ResponseWriter, r *http.Request) {
	identity, _ := auth.FromContext(r.Context())
	if !identity.CanWrite() {
		writeErr(w, http.StatusForbidden, apierr.CodeDenied, "无权限")
		return
	}
	writeJSON(w, http.StatusOK, map[string]interface{}{
		"targets": s.svc.Targets(),
	})
}

func (s *Server) addTarget(w http.ResponseWriter, r *http.Request) {
	identity, _ := auth.FromContext(r.Context())
	if !identity.CanWrite() {
		writeErr(w, http.StatusForbidden, apierr.CodeDenied, "无权限")
		return
	}
	var req struct {
		IP       string `json:"ip"`
		Hostname string `json:"hostname"`
		OS       string `json:"os"`
		Ports    string `json:"ports"`
		Protocol string `json:"protocol"`
		Tags     string `json:"tags"`
		Notes    string `json:"notes"`
		AgentID  int    `json:"agent_id"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeErr(w, http.StatusBadRequest, apierr.CodeInvalidParam, "无效 JSON")
		return
	}
	if req.IP == "" {
		writeErr(w, http.StatusBadRequest, apierr.CodeInvalidParam, "IP 不能为空")
		return
	}
	id := s.svc.TargetAdd(identity.Username, service.TargetInput{
		IP:       req.IP,
		Hostname: req.Hostname,
		OS:       req.OS,
		Ports:    req.Ports,
		Protocol: req.Protocol,
		Tags:     req.Tags,
		Notes:    req.Notes,
		AgentID:  req.AgentID,
	})
	writeJSON(w, http.StatusOK, map[string]interface{}{"status": "ok", "id": id})
}

func (s *Server) updateTarget(w http.ResponseWriter, r *http.Request) {
	identity, _ := auth.FromContext(r.Context())
	if !identity.CanWrite() {
		writeErr(w, http.StatusForbidden, apierr.CodeDenied, "无权限")
		return
	}
	id, _ := strconv.ParseInt(r.PathValue("id"), 10, 64) // 解析错误静默变 0(ISSUES I7, 本批不修)
	var req struct {
		IP       string `json:"ip"`
		Hostname string `json:"hostname"`
		OS       string `json:"os"`
		Ports    string `json:"ports"`
		Protocol string `json:"protocol"`
		Status   string `json:"status"`
		Tags     string `json:"tags"`
		Notes    string `json:"notes"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeErr(w, http.StatusBadRequest, apierr.CodeInvalidParam, "无效 JSON")
		return
	}
	if !s.svc.TargetUpdate(identity.Username, id, service.TargetInput{
		IP:       req.IP,
		Hostname: req.Hostname,
		OS:       req.OS,
		Ports:    req.Ports,
		Protocol: req.Protocol,
		Status:   req.Status,
		Tags:     req.Tags,
		Notes:    req.Notes,
	}) {
		writeErr(w, http.StatusNotFound, apierr.CodeNotFound, "目标不存在")
		return
	}
	writeJSON(w, http.StatusOK, map[string]string{"status": "ok"})
}

func (s *Server) deleteTarget(w http.ResponseWriter, r *http.Request) {
	identity, _ := auth.FromContext(r.Context())
	if !identity.CanWrite() {
		writeErr(w, http.StatusForbidden, apierr.CodeDenied, "无权限")
		return
	}
	id, _ := strconv.ParseInt(r.PathValue("id"), 10, 64) // 解析错误静默变 0(ISSUES I7, 本批不修)
	if !s.svc.TargetDelete(identity.Username, id) {
		writeErr(w, http.StatusNotFound, apierr.CodeNotFound, "目标不存在")
		return
	}
	writeJSON(w, http.StatusOK, map[string]string{"status": "ok"})
}

func (s *Server) clearTargets(w http.ResponseWriter, r *http.Request) {
	identity, _ := auth.FromContext(r.Context())
	if !identity.IsAdmin() {
		writeErr(w, http.StatusForbidden, apierr.CodeDenied, "仅 admin 可清空目标")
		return
	}
	s.svc.TargetClear(identity.Username)
	writeJSON(w, http.StatusOK, map[string]string{"status": "ok"})
}
