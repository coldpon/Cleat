package api

import (
	"encoding/json"
	"net/http"
	"strconv"
	"strings"

	"cleat/teamserver/internal/apierr"
	"cleat/teamserver/internal/auth"
)

// GET /api/listeners 运行中的监听器列表(TCP/HTTPS/DNS/WSS/QUIC)。
//
// 授权: 列表含地址/端口/协议(主机清单类信息), 与 credentials/targets 读端点同款收敛 ——
// viewer 只读但拒绝查看; 前端 MENU_ROLES 亦仅对 admin/operator 展示 listeners 页,
// 此处守卫保证"前端隐藏 ⊇ 服务端拒绝"的包含关系成立。
func (s *Server) listListeners(w http.ResponseWriter, r *http.Request) {
	identity, _ := auth.FromContext(r.Context())
	if !identity.CanWrite() {
		writeErr(w, http.StatusForbidden, apierr.CodeDenied, "viewer 无权限查看监听器")
		return
	}
	writeJSON(w, http.StatusOK, map[string]interface{}{
		"listeners": s.svc.Listeners(),
	})
}

// POST /api/listeners 动态启动一个监听器。
// body: {"protocol":"tcp"|"https"|"dns"|"wss"|"quic", "address":"0.0.0.0:5555"} 或 {"protocol":..., "port":5555}。
//   - tcp:   会话式长连接监听, 加密/伪装用服务端默认配置;
//   - https: 请求驱动 beacon 监听(GET /poll 探活, POST /upload 上传), 自签证书(内存生成);
//   - dns:   请求驱动 beacon 监听(迷你 DNS 服务器/UDP), 需额外提供 domain(与 Agent --domain 一致);
//   - wss:   HTTPS 监听 + WebSocket 路由(骑同一自签证书), 可选 ws_path(默认 /ws, 需与 Agent wss_url 路径一致);
//   - quic:  beacon 监听(quic-go/UDP + 自签 TLS, ALPN=cleat-quic), 无需额外参数。
//
// 授权: viewer 只读, 拒绝管理; operator/admin 可写。
func (s *Server) addListener(w http.ResponseWriter, r *http.Request) {
	identity, _ := auth.FromContext(r.Context())
	if !identity.CanWrite() {
		writeErr(w, http.StatusForbidden, apierr.CodeDenied, "viewer 无权限管理监听器")
		return
	}
	var req struct {
		Address  string `json:"address"`
		Port     int    `json:"port"`
		Protocol string `json:"protocol"` // "tcp"(默认) / "https" / "dns" / "wss" / "quic"
		Domain   string `json:"domain"`   // dns 必填: 查询域(与 Agent --domain 一致)
		WSPath   string `json:"ws_path"`  // wss 可选: WebSocket 升级路径(空 = /ws)
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeErr(w, http.StatusBadRequest, apierr.CodeInvalidParam, "无效 JSON")
		return
	}
	addr := req.Address
	if addr == "" {
		if req.Port <= 0 || req.Port > 65535 {
			writeErr(w, http.StatusBadRequest, apierr.CodeInvalidParam, "缺少 address 或 port 无效")
			return
		}
		addr = "0.0.0.0:" + strconv.Itoa(req.Port)
	}
	proto := strings.ToLower(strings.TrimSpace(req.Protocol))
	if proto == "" {
		proto = "tcp"
	}
	if err := s.svc.AddListener(identity.Username, proto, addr, req.Domain, req.WSPath); err != nil {
		serviceErr(w, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]interface{}{"address": addr, "protocol": proto, "status": "running"})
}

// DELETE /api/listeners/{addr} 停止监听器。
// addr 为 URL path 参数, 可能含冒号(0.0.0.0:5555)。
func (s *Server) deleteListener(w http.ResponseWriter, r *http.Request) {
	identity, _ := auth.FromContext(r.Context())
	if !identity.CanWrite() {
		writeErr(w, http.StatusForbidden, apierr.CodeDenied, "viewer 无权限管理监听器")
		return
	}
	addr := strings.TrimPrefix(r.PathValue("addr"), ":")
	if addr == "" {
		writeErr(w, http.StatusNotFound, apierr.CodeInvalidParam, "缺少监听器地址")
		return
	}
	if err := s.svc.RemoveListener(identity.Username, addr); err != nil {
		writeErr(w, http.StatusNotFound, apierr.CodeNotFound, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, map[string]interface{}{"ok": true})
}
