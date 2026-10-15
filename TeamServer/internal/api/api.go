// Package api HTTP 管理层: 路由/认证/限流/SSE 流等传输与呈现职责。
// 业务一律经 service 层(第 2 批结构手术, ): handler 只做参数转换不做业务。
package api

import (
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"io"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"cleat/teamserver/internal/apierr"
	"cleat/teamserver/internal/auth"
	"cleat/teamserver/internal/eventbus"
	"cleat/teamserver/internal/service"
)

// Server HTTP API。
type Server struct {
	svc       *service.Service
	bus       *eventbus.Hub // SSE 事件源(呈现层职责, 留在 api)
	token     string        // bearer 令牌; 空=无认证(兼容旧逻辑, 视为内置 admin 兜底)
	authH     *auth.Authenticator
	staticDir string // 前端静态文件目录(空=不托管前端)
}

// New 创建 HTTP API Server。
func New(svc *service.Service, bus *eventbus.Hub, token string) *Server {
	return &Server{svc: svc, bus: bus, token: token, authH: svc.Auth()}
}

// SetStaticDir 设置前端静态文件目录。
func (s *Server) SetStaticDir(dir string) {
	s.staticDir = dir
}

// Handler 返回 http.Handler(路由 + CORS + 认证 + 静态文件托管)。
func (s *Server) Handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /api/sessions", s.listSessions)
	mux.HandleFunc("GET /api/tasks/{id}", s.taskDetail)
	mux.HandleFunc("POST /api/sessions/{id}/command", s.sendCommand)
	mux.HandleFunc("POST /api/sessions/{id}/task", s.submitTask)
	mux.HandleFunc("POST /api/sessions/{id}/upload-chunk", s.uploadChunk)
	mux.HandleFunc("POST /api/sessions/{id}/hybrid-command", s.hybridCommand)
	mux.HandleFunc("GET /api/sessions/{id}/events", s.sessionEvents)
	mux.HandleFunc("GET /api/topology", s.topology)
	mux.HandleFunc("GET /api/events", s.globalEvents)
	mux.HandleFunc("GET /api/operators", s.listOperators)
	mux.HandleFunc("POST /api/operators", s.addOperator)
	mux.HandleFunc("DELETE /api/operators/{name}", s.removeOperator)
	mux.HandleFunc("POST /api/operators/{name}/password", s.changePassword)
	mux.HandleFunc("POST /api/operators/{name}/toggle", s.toggleOperator)
	mux.HandleFunc("POST /api/login", s.login)
	// SSE 短 token (C4)
	mux.HandleFunc("GET /api/sse-token", s.sseToken)
	// 模块仓库
	mux.HandleFunc("GET /api/modules", s.moduleList)
	mux.HandleFunc("POST /api/modules", s.moduleUpload)
	mux.HandleFunc("DELETE /api/modules/{name}", s.moduleDelete)
	mux.HandleFunc("POST /api/sessions/{id}/module-load", s.moduleLoad)
	// 凭据管理
	mux.HandleFunc("GET /api/credentials", s.listCredentials)
	mux.HandleFunc("POST /api/credentials", s.addCredential)
	mux.HandleFunc("DELETE /api/credentials/{id}", s.deleteCredential)
	mux.HandleFunc("DELETE /api/credentials", s.clearCredentials)
	// 目标管理
	mux.HandleFunc("GET /api/targets", s.listTargets)
	mux.HandleFunc("POST /api/targets", s.addTarget)
	mux.HandleFunc("PUT /api/targets/{id}", s.updateTarget)
	mux.HandleFunc("DELETE /api/targets/{id}", s.deleteTarget)
	mux.HandleFunc("DELETE /api/targets", s.clearTargets)
	// 监听器管理(动态启动/停止 TCP listener)
	mux.HandleFunc("GET /api/listeners", s.listListeners)
	mux.HandleFunc("POST /api/listeners", s.addListener)
	mux.HandleFunc("DELETE /api/listeners/{addr}", s.deleteListener)
	mux.HandleFunc("GET /api/payloads/stubs", s.listPayloadStubs)
	mux.HandleFunc("POST /api/payloads/generate", s.generatePayload)
	mux.HandleFunc("GET /api/payloads", s.listPayloads)
	mux.HandleFunc("GET /api/payloads/{name}/download", s.downloadPayload)
	mux.HandleFunc("DELETE /api/payloads/{name}", s.deletePayload)
	// SOCKS 出口(pivot)管理
	mux.HandleFunc("GET /api/pivots", s.listPivots)
	mux.HandleFunc("POST /api/pivots", s.addPivot)
	mux.HandleFunc("DELETE /api/pivots/{addr}", s.deletePivot)
	mux.HandleFunc("POST /api/pivots/entry", s.addEntry)
	// 反向端口转发(rportfwd)管理
	mux.HandleFunc("GET /api/rportfwds", s.listRportfwds)
	mux.HandleFunc("POST /api/rportfwds", s.addRportfwd)
	mux.HandleFunc("DELETE /api/rportfwds", s.deleteRportfwd)
	// 审计与事件历史
	mux.HandleFunc("GET /api/audit", s.auditList)
	mux.HandleFunc("GET /api/events/history", s.eventsHistory)

	// API 路由走认证 + 安全头
	apiHandler := securityHeaders(cors(s.auth(mux)))

	// 如果没有配置静态文件目录,只返回 API handler
	if s.staticDir == "" {
		return apiHandler
	}

	// 静态文件 + SPA fallback: /api/* 走 API, 其余走静态文件
	fs := http.FileServer(http.Dir(s.staticDir))
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		// API 路由走认证
		if len(r.URL.Path) >= 4 && r.URL.Path[:4] == "/api" {
			apiHandler.ServeHTTP(w, r)
			return
		}
		// 尝试打开静态文件; 不存在则回退到 index.html(SPA 路由)
		path := r.URL.Path
		if path == "/" {
			path = "/index.html"
		}
		fullPath := filepath.Join(s.staticDir, path)
		if _, err := os.Stat(fullPath); err == nil {
			fs.ServeHTTP(w, r)
			return
		}
		// SPA fallback: index.html
		r.URL.Path = "/"
		fs.ServeHTTP(w, r)
	})
}

// loginLimiter 登录限速: per-IP, 5 次/60s 窗口 (C7)
var (
	loginMu   sync.Mutex
	loginHits = map[string][]time.Time{}
)

// clientIP 从 RemoteAddr 提取裸 IP: 限速键曾用
// "IP:端口"整串 —— 每次新连接端口不同, 计数永不累积, 5 次/60s 形同虚设。
func clientIP(remoteAddr string) string {
	host, _, err := net.SplitHostPort(remoteAddr)
	if err != nil {
		return remoteAddr // 无端口形态(测试/unix socket)原样返回
	}
	return host
}

func loginAllowed(ip string) bool {
	loginMu.Lock()
	defer loginMu.Unlock()
	now := time.Now()
	cutoff := now.Add(-60 * time.Second)
	hits := loginHits[ip][:0]
	for _, t := range loginHits[ip] {
		if t.After(cutoff) {
			hits = append(hits, t)
		}
	}
	// 防泄漏: 清空后的条目删除, map 不无界增长(曾只增不删)
	if len(hits) == 0 {
		delete(loginHits, ip)
	} else {
		loginHits[ip] = hits
	}
	if len(hits) >= 5 {
		return false
	}
	loginHits[ip] = append(hits, now)
	return true
}

// apiLimiter 已认证客户端限流: per-operator, 50 req/s 滑动窗。
// 背景: 第 0 批只做了 login 限速(未认证爆破防护), 认证后的 API 请求此前零限流 ——
// 失控的前端循环/被盗 token 可以无限制打 API, 且 sendCommand/taskSubmit 这类
// Agent 往返端点每请求占会话 cmdMu, 高频会放大为整会话排队。
// 键 = Username(认证后 principal): 同一操作员多端共享配额; 被盗 token 与受害者
// 同桶 —— 限流不做细粒度溯源, 那是审计日志的职责。
// 开发态(无认证兜底身份)豁免: 本地部署模型不防自己人。
// 阈值 50/s: 页面加载并发拉 ~10 个请求 + 批量操作连发, 常态远够; 防的是失控循环。
// 防泄漏(照抄 A1-1 教训): 空条目删除, map 不无界增长。
var (
	apiRateMu     sync.Mutex
	apiRateHits   = map[string][]time.Time{}
	apiRateLimit  = 50
	apiRateWindow = time.Second
)

func apiAllowed(user string) bool {
	apiRateMu.Lock()
	defer apiRateMu.Unlock()
	now := time.Now()
	cutoff := now.Add(-apiRateWindow)
	hits := apiRateHits[user][:0]
	for _, t := range apiRateHits[user] {
		if t.After(cutoff) {
			hits = append(hits, t)
		}
	}
	if len(hits) == 0 {
		delete(apiRateHits, user)
	} else {
		apiRateHits[user] = hits
	}
	if len(hits) >= apiRateLimit {
		return false
	}
	apiRateHits[user] = append(hits, now)
	return true
}

// checkRate 已认证请求的统一限流闸门: 超额回 429 + Retry-After。
func checkRate(w http.ResponseWriter, id auth.Identity) bool {
	if apiAllowed(id.Username) {
		return true
	}
	w.Header().Set("Retry-After", "1")
	writeErr(w, http.StatusTooManyRequests, apierr.CodeResourceExhausted, "请求过于频繁, 请稍后再试")
	return false
}

// POST /api/login 密码登录: 返回操作员身份与 Bearer 令牌(前端存 localStorage)。
// 公开端点(由 auth 中间件放行)。
func (s *Server) login(w http.ResponseWriter, r *http.Request) {
	if !loginAllowed(clientIP(r.RemoteAddr)) {
		writeErr(w, http.StatusTooManyRequests, apierr.CodeResourceExhausted, "登录尝试过多, 请 1 分钟后再试")
		return
	}
	var req struct {
		Username string `json:"username"`
		Password string `json:"password"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeErr(w, http.StatusBadRequest, apierr.CodeInvalidParam, "无效 JSON")
		return
	}
	id, token, ok := s.authH.Login(req.Username, req.Password)
	if !ok {
		writeErr(w, http.StatusUnauthorized, apierr.CodeUnauthenticated, "用户名或密码错误")
		return
	}
	writeJSON(w, http.StatusOK, map[string]interface{}{
		"token": token, "username": id.Username, "role": id.Role,
	})
}

// sseTokStore SSE 短 token 存储 (C4): 5 分钟过期, 一次性。
var (
	sseTokMu    sync.Mutex
	sseTokStore = map[string]sseTokEntry{}
)

type sseTokEntry struct {
	identity auth.Identity
	expiry   time.Time
}

// GET /api/sse-token 换 SSE 短 token (5 分钟有效, 单次 SSE 连接用)。
func (s *Server) sseToken(w http.ResponseWriter, r *http.Request) {
	id, ok := s.authH.ContextIdentity(r)
	if !ok {
		writeErr(w, http.StatusUnauthorized, apierr.CodeUnauthenticated, "unauthorized")
		return
	}
	b := make([]byte, 24)
	if _, err := io.ReadFull(rand.Reader, b); err != nil {
		writeErr(w, http.StatusInternalServerError, apierr.CodeInternalFailed, "rand")
		return
	}
	tok := hex.EncodeToString(b)
	sseTokMu.Lock()
	sseTokStore[tok] = sseTokEntry{identity: id, expiry: time.Now().Add(5 * time.Minute)}
	sseTokMu.Unlock()
	writeJSON(w, http.StatusOK, map[string]interface{}{"token": tok, "expires_in": 300})
}

// sseIdentityFromShortToken 检查 ?token= 是否为有效 SSE 短 token。
func sseIdentityFromShortToken(tok string) (auth.Identity, bool) {
	if tok == "" {
		return auth.Identity{}, false
	}
	sseTokMu.Lock()
	defer sseTokMu.Unlock()
	e, ok := sseTokStore[tok]
	if !ok {
		return auth.Identity{}, false
	}
	if time.Now().After(e.expiry) {
		delete(sseTokStore, tok)
		return auth.Identity{}, false
	}
	delete(sseTokStore, tok) // 一次性
	return e.identity, true
}

// 原先永不回收 → 无界增长)。由 main.go 清理 sweeper 同频调用。
func PruneSSETokens(now time.Time) {
	sseTokMu.Lock()
	defer sseTokMu.Unlock()
	for tok, e := range sseTokStore {
		if now.After(e.expiry) {
			delete(sseTokStore, tok)
		}
	}
}

// viewerBlockedEvents viewer 角色不可经 SSE 读取的敏感事件类型:
// /api/credentials 端点 admin-only, 若事件流全量放行, viewer 可从这条旁路读到
// 同样的凭据/键盘记录 —— admin-only 保护被绕过。终端输出/剪贴板同理属机密数据。
// viewer 的界面本就是只读概览, 不消费这四类数据; operator/admin 不受影响。
var viewerBlockedEvents = map[string]bool{
	eventbus.EventCredential:     true,
	eventbus.EventKeylog:         true,
	eventbus.EventTerminalOutput: true,
	eventbus.EventClipboard:      true,
}

// auth 中间件: 解析 Bearer token 为操作员身份并写入 context。
// 未配置任何 token/操作员时(开发态)放行并给只读兜底身份。
// /api/login 为公开端点(密码登录), 不要求已在请求中带 token。
func (s *Server) auth(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		// 请求体上限: 曾 json.NewDecoder(r.Body) 无上限 —— 超大请求体
		// 耗尽内存。64MB 覆盖二进制分块上传/模块 DLL 的合法上限, 拒掉 DoS 量级。
		r.Body = http.MaxBytesReader(w, r.Body, 64<<20)
		if r.URL.Path == "/api/login" {
			next.ServeHTTP(w, r)
			return
		}
		if s.token == "" && s.authH.OperatorCount() == 0 {
			// 开发态: 无认证, 提供 viewer 只读身份(不遮蔽命令能力, 防误部署被任意下发)
			// 开发态豁免限流(apiLimiter 注释)
			s.svc.DevModeWarn()
			ctx := auth.WithIdentity(r.Context(), auth.Identity{Username: "unknown", Role: service.RoleViewer, Authenticated: true})
			next.ServeHTTP(w, r.WithContext(ctx))
			return
		}
		//  ?token= 可能是 SSE 短 token (一次性, 5 分钟) —— 仅 events 端点放行
		// (TS-3: 曾对所有端点生效, 泄露面过大; 且非 events 请求会误消耗一次性 token)
		if strings.HasSuffix(r.URL.Path, "/events") {
			if st, ok2 := sseIdentityFromShortToken(r.URL.Query().Get("token")); ok2 {
				if !checkRate(w, st) {
					return
				}
				next.ServeHTTP(w, r.WithContext(auth.WithIdentity(r.Context(), st)))
				return
			}
		}
		id, ok := s.authH.ContextIdentity(r)
		if !ok {
			writeErr(w, http.StatusUnauthorized, apierr.CodeUnauthenticated, "unauthorized")
			return
		}
		if !checkRate(w, id) {
			return
		}
		next.ServeHTTP(w, r.WithContext(auth.WithIdentity(r.Context(), id)))
	})
}

// securityHeaders 安全响应头 (C8): CSP / X-Frame-Options / nosniff / Referrer-Policy。
// HSTS 仅在 TLS 下有意义, 这里不强制(本地 dev 多为 HTTP)。
func securityHeaders(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		h := w.Header()
		h.Set("X-Content-Type-Options", "nosniff")
		h.Set("X-Frame-Options", "DENY")
		h.Set("Referrer-Policy", "no-referrer")
		h.Set("Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data: blob:; font-src 'self'; connect-src 'self' ws: wss:; media-src 'self' blob:")
		next.ServeHTTP(w, r)
	})
}

// allowedOrigins CORS 白名单 (C3): 仅允许本地 dev 前端, 不允许任意源。
// 生产同域部署(8080 托管 dist)时浏览器发同源请求, 不带 Origin, CORS 不触发。
var allowedOrigins = map[string]bool{
	"http://127.0.0.1:5173": true,
	"http://localhost:5173": true,
}

func cors(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		origin := r.Header.Get("Origin")
		if allowedOrigins[origin] {
			w.Header().Set("Access-Control-Allow-Origin", origin)
			w.Header().Set("Vary", "Origin")
		}
		w.Header().Set("Access-Control-Allow-Methods", "GET, POST, OPTIONS, PUT, DELETE")
		w.Header().Set("Access-Control-Allow-Headers", "Content-Type, Authorization")
		if r.Method == http.MethodOptions {
			w.WriteHeader(http.StatusNoContent)
			return
		}
		next.ServeHTTP(w, r)
	})
}

func writeJSON(w http.ResponseWriter, code int, v interface{}) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(code)
	_ = json.NewEncoder(w).Encode(v)
}

// GET /api/sessions 会话列表
func (s *Server) listSessions(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, http.StatusOK, map[string]interface{}{"sessions": s.svc.Sessions()})
}

// POST /api/sessions/{id}/command 下发命令并同步等待响应。
// body: {"cmd","params","mode"(可选 "pull")}
// 授权: viewer 只读, 拒绝下发; operator/admin 可写。
func (s *Server) sendCommand(w http.ResponseWriter, r *http.Request) {
	id := pathInt(r, "id")
	if id <= 0 {
		writeErr(w, http.StatusNotFound, apierr.CodeNotFound, "invalid session id")
		return
	}
	identity, _ := auth.FromContext(r.Context())
	if !identity.CanWrite() {
		writeErr(w, http.StatusForbidden, apierr.CodeDenied, "viewer 无权限下发命令")
		return
	}
	var req struct {
		Cmd    string                 `json:"cmd"`
		Params map[string]interface{} `json:"params"`
		Mode   string                 `json:"mode"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeErr(w, http.StatusBadRequest, apierr.CodeInvalidParam, "无效 JSON: "+err.Error())
		return
	}
	if req.Cmd == "" {
		writeErr(w, http.StatusBadRequest, apierr.CodeInvalidParam, "缺少 cmd")
		return
	}
	resp, err := s.svc.Command(identity.Username, id, req.Cmd, req.Params, req.Mode)
	if err != nil {
		// 收敛点: ErrNoSession→404, 其余→500(与旧行为一致); 码按语义标注
		serviceErr(w, err)
		return
	}
	writeJSON(w, http.StatusOK, resp)
}

// sseStream 把事件通道写为 SSE 流(通用)。
func (s *Server) sseStream(w http.ResponseWriter, r *http.Request, sub chan eventbus.Event) {
	flusher, ok := w.(http.Flusher)
	if !ok {
		writeErr(w, http.StatusInternalServerError, apierr.CodeInternalFailed, "no sse")
		return
	}
	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-cache")
	w.WriteHeader(http.StatusOK)
	// 立即写初始注释帧并 Flush: 某些代理(如 Vite8 内置 http-proxy-3)直到收到
	// 第一个数据块才转发响应头, 无初始帧的 SSE 会被代理挂起 → 前端 EventSource
	// 永远 CONNECTING。注释帧(: xxx)符合 SSE 规范, 客户端自动忽略。
	_, _ = io.WriteString(w, ": connected\n\n")
	flusher.Flush()

	defer func() {
		// 全局订阅 sessionID=0; 会话级由调用方决定退订键
	}()
	// 周期心跳: 无事件时每 20s 发注释帧, 防止空闲被中间代理/负载均衡掐断。
	heartbeat := time.NewTicker(20 * time.Second)
	defer heartbeat.Stop()
	for {
		select {
		case ev, ok := <-sub:
			if !ok {
				return
			}
			// 保护被事件流旁路绕过的洞。非 viewer 身份不过滤。
			if viewerBlockedEvents[ev.Type] {
				if id, okID := auth.FromContext(r.Context()); !okID || id.Role == service.RoleViewer {
					continue
				}
			}
			data, _ := json.Marshal(ev)
			if len(data) > 0 {
				if _, err := w.Write(append([]byte("data: "), append(data, '\n', '\n')...)); err != nil {
					return
				}
				flusher.Flush()
			}
		case <-heartbeat.C:
			if _, err := io.WriteString(w, ": ping\n\n"); err != nil {
				return
			}
			flusher.Flush()
		case <-r.Context().Done():
			return
		}
	}
}

// GET /api/events 全局事件流(会话上下线)。
func (s *Server) globalEvents(w http.ResponseWriter, r *http.Request) {
	sub := s.bus.SubscribeGlobal()
	defer s.bus.Unsubscribe(0, sub)
	s.sseStream(w, r, sub)
}

// GET /api/sessions/{id}/events 会话级事件流。
func (s *Server) sessionEvents(w http.ResponseWriter, r *http.Request) {
	id := pathInt(r, "id")
	sub := s.bus.SubscribeSession(id)
	defer s.bus.Unsubscribe(id, sub)
	s.sseStream(w, r, sub)
}

func pathInt(r *http.Request, key string) int {
	// Go 1.22 PathValue
	v := r.PathValue(key)
	n := 0
	for _, c := range v {
		if c < '0' || c > '9' {
			return 0
		}
		n = n*10 + int(c-'0')
	}
	return n
}

// chunkIndexInt 解析字符串为 int(SQLite/query 传字符串); 失败返回 0。
func chunkIndexInt(s string) int {
	n := 0
	for _, c := range s {
		if c < '0' || c > '9' {
			return 0
		}
		n = n*10 + int(c-'0')
	}
	return n
}

// POST /api/sessions/{id}/hybrid-command?cmd=ModuleLoad&module_id=xxx
// body: 原始二进制(模块 DLL 字节等)。服务端包装为混合帧(JSON头 + '\0' + 二进制)转发给 Agent。
// 通用带二进制命令通道(模块下发用; upload-chunk 是 FileUploadData 专用)。
func (s *Server) hybridCommand(w http.ResponseWriter, r *http.Request) {
	id := pathInt(r, "id")
	identity, _ := auth.FromContext(r.Context())
	if !identity.CanWrite() {
		writeErr(w, http.StatusForbidden, apierr.CodeDenied, "viewer 无权限下发命令")
		return
	}
	cmd := r.URL.Query().Get("cmd")
	if cmd == "" {
		writeErr(w, http.StatusBadRequest, apierr.CodeInvalidParam, "缺少 cmd")
		return
	}
	body, err := io.ReadAll(io.LimitReader(r.Body, 50*1024*1024))
	if err != nil || len(body) == 0 {
		writeErr(w, http.StatusBadRequest, apierr.CodeInvalidParam, "空请求体或读取失败")
		return
	}
	// 除 cmd 外的 query 参数全部进 params(如 module_id/type/platform)
	params := map[string]interface{}{}
	for k, vs := range r.URL.Query() {
		if k == "cmd" {
			continue
		}
		params[k] = vs[0]
	}
	// ModuleLoad 强制携带仓库哈希(sha256 闭环, 见 EnsureModuleSha256)
	if err := s.svc.EnsureModuleSha256(cmd, params); err != nil {
		writeErr(w, http.StatusBadRequest, apierr.CodeInvalidParam, err.Error())
		return
	}
	resp, err := s.svc.HybridUpload(id, cmd, params, body)
	if err != nil {
		writeInternal(w, apierr.CodeInternalFailed, "内部错误", err)
		return
	}
	writeJSON(w, http.StatusOK, resp)
}
