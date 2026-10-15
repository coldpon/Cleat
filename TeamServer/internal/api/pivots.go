package api

import (
	"encoding/json"
	"net/http"
	"strconv"
	"strings"

	"cleat/teamserver/internal/apierr"
	"cleat/teamserver/internal/auth"
)

// GET /api/pivots SOCKS 出口列表(结构化: session_id/local_addr/port/entry_*/status)。
func (s *Server) listPivots(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, map[string]interface{}{"pivots": s.svc.PivotInfos()})
}

// POST /api/pivots 为指定会话启动 SOCKS 出口。
// body: {"session_id":154,"port":1080,"entry_host":"","entry_port":0}
// entry_host/entry_port 非空时为多跳链式。
func (s *Server) addPivot(w http.ResponseWriter, r *http.Request) {
	identity, _ := auth.FromContext(r.Context())
	if !identity.CanWrite() {
		writeErr(w, http.StatusForbidden, apierr.CodeDenied, "viewer 无权限管理隧道")
		return
	}
	var req struct {
		SessionID int    `json:"session_id"`
		Port      int    `json:"port"`
		EntryHost string `json:"entry_host"`
		EntryPort int    `json:"entry_port"`
		EntryUser string `json:"entry_user"` // 下一跳入口 RFC1929 凭据(空 = 无鉴权入口)
		EntryPass string `json:"entry_pass"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeErr(w, http.StatusBadRequest, apierr.CodeInvalidParam, "无效 JSON")
		return
	}
	if req.SessionID <= 0 || req.Port <= 0 || req.Port > 65535 {
		writeErr(w, http.StatusBadRequest, apierr.CodeInvalidParam, "session_id/port 无效")
		return
	}
	if err := s.svc.PivotAdd(identity.Username, req.SessionID, req.Port,
		req.EntryHost, req.EntryPort, req.EntryUser, req.EntryPass); err != nil {
		serviceErr(w, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]interface{}{
		"local_addr": "127.0.0.1:" + strconv.Itoa(req.Port),
		"status":     "running",
	})
}

// DELETE /api/pivots/{addr} 停止 SOCKS 出口。addr 形如 127.0.0.1:1080。
func (s *Server) deletePivot(w http.ResponseWriter, r *http.Request) {
	identity, _ := auth.FromContext(r.Context())
	if !identity.CanWrite() {
		writeErr(w, http.StatusForbidden, apierr.CodeDenied, "viewer 无权限管理隧道")
		return
	}
	addr := strings.TrimPrefix(r.PathValue("addr"), ":")
	if addr == "" {
		writeErr(w, http.StatusNotFound, apierr.CodeInvalidParam, "缺少出口地址")
		return
	}
	if err := s.svc.PivotRemove(identity.Username, addr); err != nil {
		writeErr(w, http.StatusNotFound, apierr.CodeNotFound, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, map[string]interface{}{"ok": true})
}
