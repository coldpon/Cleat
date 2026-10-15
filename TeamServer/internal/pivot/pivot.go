// Package pivot Pivot 转发: Server 端 SOCKS5 入口, 照搬自旧 Server/pivot.go 并适配 TeamServer 结构。
//
// 单跳: SOCKS client -> PivotStart(Agent 连内网目标) -> 双向管道
// 多跳: SOCKS client -> PivotStart(leg1: Agent A 连 Agent B 的入口端口)
//
//	     -> 透传 SOCKS CONNECT 字节(真正目标) -> B 的入口本地建连转发
//	B 的入口是 SOCKS5 服务端(PivotEntry), 上游 A 只看到 entry 地址,
//	不知道真正目标 —— 节点只知道前后一跳。
//
// 下行(Server→Agent): PivotStart/PivotClose 走 Manager.APIHybridPush(混合帧);
//
//	PivotData 走 Manager.APIHybridPush(混合帧, 非阻塞透传)。
//
// 上行(Agent→Server):  PivotData/PivotClose 是 Agent 主动推送, 由本包注册为
//
//	Manager handler(经 session.Dispatch 解析 data/params 载荷)。
//
// 会话离线(EventSessionOffline)时清理该会话所有 pivot 出口连接与 SOCKS listener。
// SOCKS5 握手协议见 socks.go。
package pivot

import (
	"crypto/rand"
	"encoding/hex"
	"fmt"
	"net"
	"sync"
	"time"

	"cleat/teamserver/internal/eventbus"
	"cleat/teamserver/internal/logx"
	"cleat/teamserver/internal/session"
)

// Pivot 管理所有 SOCKS 出口与 pivot 连接。
type Pivot struct {
	mgr *session.Manager
	bus *eventbus.Hub

	pivotsMu sync.Mutex
	pivots   map[string]*pivotConn

	listensMu sync.Mutex
	listeners map[string]*pivotListener // addr -> listener 元信息(拓扑/CLI)

	entriesMu sync.Mutex
	entries   map[int]entryCred // 入口端口 -> TS 生成的凭据(pivotchain 引用)

	rportfwdsMu sync.Mutex
	rportfwds   map[string]*rportfwdEntry // "sid:lport" -> 反向端口转发登记

	pipeEntriesMu sync.Mutex
	pipeEntries   map[string]PipeEntry // pipe_name -> 会话借道入口凭据(L1')

	virtMu      sync.Mutex
	virtualPipes map[string]*virtualPipe // "sess-<pipe_name>" -> 虚拟子会话链路
}

// entryCred 多跳入口凭据(TS 生成, entry 启动时下发, 建链时引用)。
type entryCred struct {
	User string
	Pass string
}

type pivotListener struct {
	sessionID int
	port      int
	entryHost string // non-empty = multi-hop chain
	entryPort int
	ln        net.Listener // 出口监听器; 会话离线时关闭, 释放端口与 accept 协程
}

// New 创建 pivot 管理器并注册 Agent 主动推送(PivotData/PivotClose/PivotOpened)处理。
// 同时订阅全局事件: 会话离线时清理其 pivot。
func New(mgr *session.Manager, bus *eventbus.Hub) *Pivot {
	p := &Pivot{
		mgr:       mgr,
		bus:       bus,
		pivots:    make(map[string]*pivotConn),
		listeners: make(map[string]*pivotListener),
		entries:      make(map[int]entryCred),
		rportfwds:    make(map[string]*rportfwdEntry),
		pipeEntries:  make(map[string]PipeEntry),
		virtualPipes: make(map[string]*virtualPipe),
	}
	mgr.RegisterHandler(session.CmdPivotData, p.handleData)
	mgr.RegisterHandler(session.CmdPivotClose, p.handleClose)
	mgr.RegisterHandler(session.CmdPivotOpened, p.handleOpen)

	// 订阅会话离线事件, 清理该会话的出口连接与 listener。
	sub := bus.SubscribeGlobal()
	go func() {
		defer logx.Guard("pivot-eventsub") // G3
		for ev := range sub {
			if ev.Type == eventbus.EventSessionOffline && ev.SessionID > 0 {
				p.closeSession(ev.SessionID)
			}
		}
	}()
	return p
}

// StartSOCKS 为指定会话启动 SOCKS5 监听(127.0.0.1:<port>)。
// entryHost/entryPort 非空时为多跳链式: 经该 Agent 连到下一跳的入口端口,
// entryUser/entryPass 为该入口的 RFC1929 凭据(空 = 入口无鉴权)。
func (p *Pivot) StartSOCKS(sid int, port int, entryHost string, entryPort int,
	entryUser string, entryPass string) error {
	if sid <= 0 {
		return fmt.Errorf("无效会话 #%d", sid)
	}
	addr := fmt.Sprintf("127.0.0.1:%d", port)
	ln, err := net.Listen("tcp", addr)
	if err != nil {
		return err
	}
	p.listensMu.Lock()
	p.listeners[addr] = &pivotListener{sessionID: sid, port: port, entryHost: entryHost, entryPort: entryPort, ln: ln}
	p.listensMu.Unlock()

	if entryHost != "" {
		logx.Printf("[pivot] 会话 #%d SOCKS5 %s -> 经植入物 -> 入口 %s:%d (多跳)\n", sid, addr, entryHost, entryPort)
	} else {
		logx.Printf("[pivot] 会话 #%d SOCKS5 监听 %s (经植入物转发内网流量)\n", sid, addr)
	}
	go func(lis net.Listener) {
		defer logx.Guard("pivot-socks-accept") // G3
		for {
			conn, err := lis.Accept()
			if err != nil {
				return
			}
			// 会话可能已离线, net.Conn 先于 Agent 请求建立, 由 handshake 阶段校验。
			go p.handleSocks(conn, sid, entryHost, entryPort, entryUser, entryPass)
		}
	}(ln)
	return nil
}

// Listeners 返回当前 SOCKS 出口列表("会话#N addr 单跳/多跳@host:port")。
func (p *Pivot) Listeners() []string {
	p.listensMu.Lock()
	defer p.listensMu.Unlock()
	out := make([]string, 0, len(p.listeners))
	for addr, l := range p.listeners {
		if l.entryHost != "" {
			out = append(out, fmt.Sprintf("#%d %s (chain@%s:%d)", l.sessionID, addr, l.entryHost, l.entryPort))
		} else {
			out = append(out, fmt.Sprintf("#%d %s (socks5)", l.sessionID, addr))
		}
	}
	return out
}

// ListenerInfo 描述一个 SOCKS 出口(REST API 用)。
type ListenerInfo struct {
	SessionID int    `json:"session_id"`
	LocalAddr string `json:"local_addr"`
	Port      int    `json:"port"`
	EntryHost string `json:"entry_host,omitempty"`
	EntryPort int    `json:"entry_port,omitempty"`
	Status    string `json:"status"`
}

// ListenerInfos 返回 SOCKS 出口结构化列表。
func (p *Pivot) ListenerInfos() []ListenerInfo {
	p.listensMu.Lock()
	defer p.listensMu.Unlock()
	out := make([]ListenerInfo, 0, len(p.listeners))
	for addr, l := range p.listeners {
		out = append(out, ListenerInfo{
			SessionID: l.sessionID, LocalAddr: addr, Port: l.port,
			EntryHost: l.entryHost, EntryPort: l.entryPort, Status: "running",
		})
	}
	return out
}

// Stop 停止指定地址的 SOCKS 出口(释放端口与 accept 协程)。
func (p *Pivot) Stop(addr string) error {
	p.listensMu.Lock()
	l := p.listeners[addr]
	delete(p.listeners, addr)
	p.listensMu.Unlock()
	if l == nil {
		return fmt.Errorf("pivot 出口 %s 不存在", addr)
	}
	return l.ln.Close()
}

// handleData Agent 主动推送 PivotData -> 写回 SOCKS 客户端连接。
func (p *Pivot) handleData(s *session.Session, cmd string, params map[string]interface{}, bin []byte) {
	pivotID, _ := params["pivot_id"].(string)
	if pivotID == "" {
		return
	}
	// 会话借道流量(P1'): sess- 前缀分流到虚拟子会话(net.Pipe 伪装连接),
	// 其余走既有 pivot 透传表。
	if IsSessionId(pivotID) {
		p.handleSessionData(s, pivotID, bin)
		return
	}
	p.pivotsMu.Lock()
	pc := p.pivots[pivotID]
	p.pivotsMu.Unlock()
	if pc == nil {
		return
	}

	pc.writeMu.Lock()
	defer pc.writeMu.Unlock()
	if pc.closed {
		return
	}

	// rportfwd 建连阶段: PivotOpened 已受理、目标尚未拨通, 先缓冲上行
	// 数据(拨通后在 setupRportfwdConn 里回放), 避免 TLS ClientHello
	// 之类先手数据在拨号窗口内被丢弃。超限视为失败并关闭。
	if pc.phase == "rport-pending" {
		if len(pc.pendingBuf)+len(bin) > rportfwdPendingMax {
			pc.closed = true
			logx.Printf("[rportfwd] %s pending 超限(%dB), 关闭\n", pivotID, len(pc.pendingBuf)+len(bin))
			sid := pc.sessionID
			go func() {
				defer logx.Guard("pivot-push-close") // G3
				_ = p.pushClose(pivotID, sid)
			}()
			return
		}
		pc.pendingBuf = append(pc.pendingBuf, bin...)
		return
	}

	// 多跳协商阶段: 入口先回方法选择 ack(2B), 带鉴权时再回 auth 结果(2B),
	// 最后回 CONNECT ok(10B); 各段可能分帧或合并到达, 累积缓冲后统一解析
	if pc.phase == "negotiate" {
		pc.negotiateBuf = append(pc.negotiateBuf, bin...)
		b := pc.negotiateBuf
		// 0xFF = 入口无可接受方法(凭据缺失/不匹配), 快速失败
		if len(b) >= 2 && b[1] == 0xFF {
			pc.negotiateBuf = nil
			select {
			case pc.negotiatedCh <- false:
			default:
			}
			return
		}
		need := 12 // 无鉴权: ack(2) + ok(10)
		if len(b) >= 2 && b[1] == 0x02 {
			need = 14 // RFC1929: ack(2) + auth(2) + ok(10)
		}
		if len(b) < need {
			return // 等齐
		}
		ackOK := b[0] == 0x05 && (b[1] == 0x00 || b[1] == 0x02)
		okOK := false
		off := 2
		if ackOK && b[1] == 0x02 {
			// auth 子协商结果: ver(01) status(00=成功)
			ackOK = b[2] == 0x01 && b[3] == 0x00
			off = 4
		}
		if ackOK {
			okOK = b[off] == 0x05 && b[off+1] == 0x00
		}
		if len(b) > need {
			pc.pendingBuf = append(pc.pendingBuf, b[need:]...)
		}
		pc.negotiateBuf = nil
		select {
		case pc.negotiatedCh <- (ackOK && okOK):
		default:
		}
		return
	}

	// 协商期间可能粘连的多余数据先写
	if len(pc.pendingBuf) > 0 {
		_ = pc.conn.SetWriteDeadline(time.Now().Add(30 * time.Second))
		if _, err := pc.conn.Write(pc.pendingBuf); err != nil {
			return
		}
		pc.pendingBuf = nil
	}
	_ = pc.conn.SetWriteDeadline(time.Now().Add(30 * time.Second))
	_, _ = pc.conn.Write(bin)
}

// handleClose Agent 主动推送 PivotClose: 目标连接已断开, 关闭 SOCKS 客户端连接。
func (p *Pivot) handleClose(s *session.Session, cmd string, params map[string]interface{}, binary []byte) {
	pivotID, _ := params["pivot_id"].(string)
	// sess- 前缀: Agent 侧入口关闭(StopPipeEntry) → 关虚拟子会话
	if IsSessionId(pivotID) {
		p.closeVirtual(pivotID)
		return
	}
	p.pivotsMu.Lock()
	pc := p.pivots[pivotID]
	delete(p.pivots, pivotID)
	p.pivotsMu.Unlock()
	if pc != nil {
		pc.writeMu.Lock()
		pc.closed = true
		_ = pc.conn.Close()
		pc.writeMu.Unlock()
		logx.Printf("[pivot] 会话 #%d %s 已关闭 (%v)\n", s.ID(), pivotID, params["reason"])
	}
}

// closeSession 会话掉线/关闭时清理其所有 pivot 出口连接, 并关闭其 SOCKS listener
// (释放端口与 accept 协程, 防止离线后端口残留仍被监听)。
func (p *Pivot) closeSession(sid int) {
	p.listensMu.Lock()
	var closedLn []net.Listener
	for addr, l := range p.listeners {
		if l.sessionID == sid {
			closedLn = append(closedLn, l.ln)
			delete(p.listeners, addr)
		}
	}
	p.listensMu.Unlock()
	for _, ln := range closedLn {
		if ln != nil {
			_ = ln.Close()
		}
	}

	p.pivotsMu.Lock()
	var victims []*pivotConn
	for id, pc := range p.pivots {
		if pc.sessionID == sid {
			victims = append(victims, pc)
			delete(p.pivots, id)
		}
	}
	p.pivotsMu.Unlock()

	// 该会话的 rportfwd 登记一并清除(Agent 端监听随会话失效)
	p.rportfwdsMu.Lock()
	for key, e := range p.rportfwds {
		if e.SessionID == sid {
			delete(p.rportfwds, key)
		}
	}
	p.rportfwdsMu.Unlock()

	// 会话借道级联: 父会话离线 → 其全部虚拟子会话关闭(子会话离线事件再触发
	// 它们自己的 closeSession, 递归清到链尾)
	p.closeVirtualByParent(sid)
	for _, pc := range victims {
		pc.writeMu.Lock()
		pc.closed = true
		_ = pc.conn.Close()
		pc.writeMu.Unlock()
	}
}

// ---- 多跳入口(entry)凭据管理 ----

// StartEntry 为指定会话启动多跳入口(Agent 侧 PivotEntry, SOCKS5 服务端)。
// 凭据由 TS 随机生成并随 PivotListen 下发: 有凭据的入口 Agent 默认绑
// 0.0.0.0(供跨机链路连入), 无凭据入口 Agent 默认只绑 127.0.0.1。
// 凭据按入口端口缓存, pivotchain 建链时按入口端口引用。
func (p *Pivot) StartEntry(sid int, port int) (entryCred, error) {
	if sid <= 0 {
		return entryCred{}, fmt.Errorf("无效会话 #%d", sid)
	}
	cred := entryCred{User: randHex(8), Pass: randHex(16)}
	resp, err := p.mgr.APICommand(sid, session.CmdPivotListen, map[string]interface{}{
		"port": port,
		"user": cred.User,
		"pass": cred.Pass,
	})
	if err != nil {
		return entryCred{}, err
	}
	if resp == nil || resp["status"] != "success" {
		return entryCred{}, fmt.Errorf("entry 启动失败: %v", resp)
	}
	p.entriesMu.Lock()
	p.entries[port] = cred
	p.entriesMu.Unlock()
	logx.Printf("[pivot] 会话 #%d entry 监听 :%d (鉴权 user=%s)\n", sid, port, cred.User)
	return cred, nil
}

// EntryCredFor 按入口端口取凭据(同端口多次启动以最后一次为准)。
func (p *Pivot) EntryCredFor(entryPort int) (entryCred, bool) {
	p.entriesMu.Lock()
	defer p.entriesMu.Unlock()
	c, ok := p.entries[entryPort]
	return c, ok
}

func randHex(n int) string {
	b := make([]byte, n)
	_, _ = rand.Read(b)
	return hex.EncodeToString(b)
}
