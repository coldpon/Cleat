package api

import (
	"encoding/json"
	"net/http"
	"strconv"

	"cleat/teamserver/internal/apierr"
	"cleat/teamserver/internal/auth"
	"cleat/teamserver/internal/service"
)

func (s *Server) listCredentials(w http.ResponseWriter, r *http.Request) {
	identity, _ := auth.FromContext(r.Context())
	// 防重蹈覆辙：此处曾是 IsAdmin，而写路径是 CanWrite —— 形成"operator 可写可删单条
	// 却一条读不到"的权限倒挂（能删的人看不到自己删了什么，loot 无法被 operator 使用）。
	//  按 loot 共享语义对齐：读=CanWrite（Mythic/CS operator 均可读写凭据库）。
	// 不要再单独收紧读权限；若认为凭据需要更强保护，读写一起收紧并同步前端。
	if !identity.CanWrite() {
		writeErr(w, http.StatusForbidden, apierr.CodeDenied, "无权限")
		return
	}
	writeJSON(w, http.StatusOK, map[string]interface{}{
		"credentials": s.svc.Credentials(),
	})
}

func (s *Server) addCredential(w http.ResponseWriter, r *http.Request) {
	identity, _ := auth.FromContext(r.Context())
	if !identity.CanWrite() {
		writeErr(w, http.StatusForbidden, apierr.CodeDenied, "无权限")
		return
	}
	var req struct {
		Username string `json:"username"`
		Realm    string `json:"realm"`
		CredType string `json:"cred_type"`
		Secret   string `json:"secret"`
		Host     string `json:"host"`
		Source   string `json:"source"`
		AgentID  int    `json:"agent_id"`
		Tag      string `json:"tag"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeErr(w, http.StatusBadRequest, apierr.CodeInvalidParam, "无效 JSON")
		return
	}
	id := s.svc.CredentialAdd(identity.Username, service.CredentialInput{
		Username: req.Username,
		Realm:    req.Realm,
		CredType: req.CredType,
		Secret:   req.Secret,
		Host:     req.Host,
		Source:   req.Source,
		AgentID:  req.AgentID,
		Tag:      req.Tag,
	})
	writeJSON(w, http.StatusOK, map[string]interface{}{"status": "ok", "id": id})
}

func (s *Server) deleteCredential(w http.ResponseWriter, r *http.Request) {
	identity, _ := auth.FromContext(r.Context())
	if !identity.CanWrite() {
		writeErr(w, http.StatusForbidden, apierr.CodeDenied, "无权限")
		return
	}
	id, _ := strconv.ParseInt(r.PathValue("id"), 10, 64)
	if !s.svc.CredentialDelete(identity.Username, id) {
		writeErr(w, http.StatusNotFound, apierr.CodeNotFound, "凭据不存在")
		return
	}
	writeJSON(w, http.StatusOK, map[string]string{"status": "ok"})
}

func (s *Server) clearCredentials(w http.ResponseWriter, r *http.Request) {
	identity, _ := auth.FromContext(r.Context())
	if !identity.IsAdmin() {
		writeErr(w, http.StatusForbidden, apierr.CodeDenied, "仅 admin 可清空凭据")
		return
	}
	s.svc.CredentialClear(identity.Username)
	writeJSON(w, http.StatusOK, map[string]string{"status": "ok"})
}
