package session

import (
	"crypto/tls"
	"fmt"
	"net"
	"net/http"
	"strconv"
	"strings"
	"time"

	"cleat/teamserver/internal/crypto"
	"cleat/teamserver/internal/logx"
	"cleat/teamserver/internal/protocol"
)

// listener 是一个运行中的监听器(端口 + 独立加密/伪装配置)。
// 对齐业界 teamserver 的"动态 listener 管理": 运行中可增删、每 listener 独立 profile。
// protocol 区分会话式 TCP、请求驱动式 HTTPS(beacon) 与 DNS(beacon/UDP), 生命周期管理不同。
type listener struct {
	addr     string
	protocol string // "tcp"(默认) / "https" / "dns" / "quic"
	ln       net.Listener
	httpSrv  *http.Server // 非 nil 时为 HTTPS(beacon) 监听器
	udpConn  *net.UDPConn // 非 nil 时为 DNS(beacon) 监听器
	quicH    QUICHandler  // 非 nil 时为 QUIC(beacon) 监听器(socket 由 quic-go 自持)
	cbox     *crypto.Box
	xbox     *crypto.X25519Box // 非 nil 时 TCP 通道走 X25519 会话模式(优先于 cbox)
	mimic    bool
	done     chan struct{}
}

// DNSHandler DNS listener 的报文处理接口(由 beacon 包的 DNSListener 实现)。
// 以接口注入而非直接依赖, 避免 session ← beacon 的包循环。
type DNSHandler interface {
	// Serve 在 conn 上跑服务循环, 阻塞至 conn 关闭。
	Serve(conn *net.UDPConn) error
}

// QUICHandler QUIC listener 的服务接口(由 beacon 包的 QUICListener 实现)。
// 同 DNSHandler: 接口注入避免 session ← beacon 包循环; 与 DNS 的差异在于
// QUIC 的 UDP socket 由 quic-go listener 自持(不在本包建), 故只需 Serve/Close。
type QUICHandler interface {
	// Serve 跑接受连接循环, 阻塞至 Close。
	Serve() error
	// Close 停止监听(幂等)。
	Close() error
}

// AddListener 动态启动一个监听器(port + 独立加密/伪装配置)。
// 已在监听则返回 nil(幂等)。xbox 非 nil 时 TCP 通道支持 X25519 握手。
func (m *Manager) AddListener(listenAddr string, cbox *crypto.Box, xbox *crypto.X25519Box, mimic bool) error {
	m.listenMu.Lock()
	if _, ok := m.listeners[listenAddr]; ok {
		m.listenMu.Unlock()
		return nil
	}
	m.listenMu.Unlock()

	ln, err := net.Listen("tcp", listenAddr)
	if err != nil {
		return fmt.Errorf("监听 %s 失败: %w", listenAddr, err)
	}
	lis := &listener{addr: listenAddr, protocol: "tcp", ln: ln, cbox: cbox, xbox: xbox, mimic: mimic, done: make(chan struct{})}
	m.listenMu.Lock()
	m.listeners[listenAddr] = lis
	m.listenMu.Unlock()
	mode := "明文"
	switch {
	case xbox != nil:
		mode = "X25519 握手加密"
	case cbox != nil:
		mode = "XChaCha20-Poly1305"
	}
	if mimic {
		mode += " · 伪装"
	}
	logx.Printf("[*] 会话监听 %s（%s）\n", listenAddr, mode)

	// accept 循环
	go func(lis *listener) {
		defer logx.Guard("tcp-accept") // G3: panic 兜底, 防 panic 沿 goroutine 带崩进程
		var backoff time.Duration
		for {
			conn, err := lis.ln.Accept()
			if err != nil {
				select {
				case <-lis.done:
					return
				default:
				}
				// A3-18: 持续性错误(EMFILE/句柄耗尽)曾直接 continue
				// → 忙循环打满 CPU。指数退避封顶 1s, 期间 done 关闭仍能退出。
				backoff *= 2
				if backoff < 10*time.Millisecond {
					backoff = 10 * time.Millisecond
				}
				if backoff > time.Second {
					backoff = time.Second
				}
				time.Sleep(backoff)
				continue
			}
			backoff = 0
			// 开启 TCP keepalive: 防止空闲长连接被中间设备(防火墙/NAT)或系统
			// 按 KeepAliveTime(Windows 默认 7200s=2h)回收, 造成周期性断连。
			// 短周期探测让链路持续保活。
			if tc, ok := conn.(*net.TCPConn); ok {
				_ = tc.SetKeepAlive(true)
				_ = tc.SetKeepAlivePeriod(30 * time.Second)
			}
			pc := protocol.NewConn(conn, lis.cbox, lis.xbox, lis.mimic)
			go m.AcceptConn(pc, lis.protocol) // lis.protocol: TCP 监听恒为 "tcp"
		}
	}(lis)
	return nil
}

// AddHTTPSListener 动态启动一个 HTTPS(beacon) 监听器。
// handler 为 beacon HTTP 处理器(Upload/Poll 路由); tlsCfg 通常为自签配置(Agent 侧
// https_ignore_cert=true 时跳过校验)。请求驱动通道: 命令经 POST 响应体带回, 见 beacon 包。
// tlsCfg 为 nil 时退化为纯 HTTP beacon(开发态), protocol 相应标记 "http"。
// protoOverride 非空时覆盖协议标签(如 wss = HTTPS 监听 + WebSocket 路由, 面板如实展示);
// 空 = 按 tlsCfg 自动定 "https"/"http"。已在监听则返回 nil(幂等)。
func (m *Manager) AddHTTPSListener(listenAddr string, handler http.Handler, tlsCfg *tls.Config, protoOverride string) error {
	m.listenMu.Lock()
	if _, ok := m.listeners[listenAddr]; ok {
		m.listenMu.Unlock()
		return nil
	}
	m.listenMu.Unlock()

	ln, err := net.Listen("tcp", listenAddr)
	if err != nil {
		return fmt.Errorf("监听 %s 失败: %w", listenAddr, err)
	}
	proto := "https"
	if tlsCfg == nil {
		proto = "http"
	}
	if protoOverride != "" {
		proto = protoOverride
	}
	srv := &http.Server{
		Handler:           handler,
		TLSConfig:         tlsCfg,
		ReadHeaderTimeout: 10 * time.Second,
	}
	lis := &listener{addr: listenAddr, protocol: proto, ln: ln, httpSrv: srv, done: make(chan struct{})}
	m.listenMu.Lock()
	m.listeners[listenAddr] = lis
	m.listenMu.Unlock()
	// : 原"(路径 GET /poll POST /upload)"是硬编码文案, 实际路径由调用方
	// 的 mux 注册决定(例: -beacon-url /api/check) —— 照抄曾把排障引向错误路径。
	// : 输出风格统一为 [*] 前缀(原 [HTTPS] 方括号标签与启动日志风格不一)。
	logx.Printf("[*] Beacon 监听 %s（%s）\n", listenAddr, strings.ToLower(proto))

	go func(lis *listener) {
		defer logx.Guard("https-serve") // G3
		var err error
		if tlsCfg != nil {
			err = srv.Serve(tls.NewListener(ln, tlsCfg))
		} else {
			err = srv.Serve(ln)
		}
		if err != nil && err != http.ErrServerClosed {
			select {
			case <-lis.done:
			default:
				logx.Printf("[HTTPS] 监听 %s 退出: %v\n", listenAddr, err)
			}
		}
	}(lis)
	return nil
}

// AddDNSListener 动态启动一个 DNS(beacon) 监听器。
// handler 为迷你 DNS 服务器(解析 qname 分片 → 会话处理 → TXT 回带命令);
// 请求驱动通道, 与 HTTPS beacon 共用会话层。已在监听则返回 nil(幂等)。
func (m *Manager) AddDNSListener(listenAddr string, handler DNSHandler) error {
	m.listenMu.Lock()
	if _, ok := m.listeners[listenAddr]; ok {
		m.listenMu.Unlock()
		return nil
	}
	m.listenMu.Unlock()

	udpAddr, err := net.ResolveUDPAddr("udp", listenAddr)
	if err != nil {
		return fmt.Errorf("解析 UDP 地址 %s 失败: %w", listenAddr, err)
	}
	conn, err := net.ListenUDP("udp", udpAddr)
	if err != nil {
		return fmt.Errorf("监听 %s 失败: %w", listenAddr, err)
	}
	lis := &listener{addr: listenAddr, protocol: "dns", udpConn: conn, done: make(chan struct{})}
	m.listenMu.Lock()
	m.listeners[listenAddr] = lis
	m.listenMu.Unlock()

	go func(lis *listener) {
		defer logx.Guard("dns-serve") // G3
		if err := handler.Serve(conn); err != nil {
			select {
			case <-lis.done:
			default:
				logx.Printf("[DNS] 监听 %s 退出: %v\n", listenAddr, err)
			}
		}
	}(lis)
	return nil
}

// AddQUICListener 动态启动一个 QUIC beacon 监听器(quic-go/UDP)。
// handler 自带 quic-go listener(内含 UDP socket 与自签 TLS + ALPN), 本函数只做
// 登记与服务循环托管。会话式通道: 桥接后与 TCP/WS 同族, 与 HTTPS/DNS 共用会话层。
// 已在监听则返回 nil(幂等)。
func (m *Manager) AddQUICListener(listenAddr string, handler QUICHandler) error {
	m.listenMu.Lock()
	if _, ok := m.listeners[listenAddr]; ok {
		m.listenMu.Unlock()
		return nil
	}
	m.listenMu.Unlock()

	lis := &listener{addr: listenAddr, protocol: "quic", quicH: handler, done: make(chan struct{})}
	m.listenMu.Lock()
	m.listeners[listenAddr] = lis
	m.listenMu.Unlock()
	logx.Printf("[QUIC] beacon 监听 %s\n", listenAddr)

	go func(lis *listener) {
		defer logx.Guard("quic-serve") // G3
		err := handler.Serve()
		select {
		case <-lis.done:
			// 主动停止: 静默
		default:
			if err != nil {
				logx.Printf("[QUIC] 监听 %s 退出: %v\n", listenAddr, err)
			}
		}
	}(lis)
	return nil
}

// RemoveListener 动态停止一个监听器。
// CryptoForAddr 返回指定监听地址的加密盒与会话包装参数。
// 会话借道(pivot 管道)的虚拟会话用: 子 Agent 的会话 Conn 需要与其部署
// listener 一致的加解密盒, 才能解出经父节点透传上来的密文帧。
// mimic 恒 false(伪装头只存在于真实落线, 管道流无伪装头)。
func (m *Manager) CryptoForAddr(listenAddr string) (*crypto.Box, *crypto.X25519Box, bool) {
	m.listenMu.Lock()
	defer m.listenMu.Unlock()
	lis, ok := m.listeners[listenAddr]
	if !ok {
		return nil, nil, false
	}
	return lis.cbox, lis.xbox, true
}

func (m *Manager) RemoveListener(listenAddr string) error {
	m.listenMu.Lock()
	lis := m.listeners[listenAddr]
	delete(m.listeners, listenAddr)
	m.listenMu.Unlock()
	if lis == nil {
		return fmt.Errorf("监听器 %s 不存在", listenAddr)
	}
	close(lis.done)
	if lis.httpSrv != nil {
		// HTTPS: 关闭 http.Server 会连带关闭底层 listener 并断开活动连接
		return lis.httpSrv.Close()
	}
	if lis.udpConn != nil {
		return lis.udpConn.Close()
	}
	if lis.quicH != nil {
		return lis.quicH.Close()
	}
	return lis.ln.Close()
}

// ListListeners 返回当前运行的监听器地址列表。
func (m *Manager) ListListeners() []string {
	m.listenMu.Lock()
	defer m.listenMu.Unlock()
	out := make([]string, 0, len(m.listeners))
	for addr := range m.listeners {
		out = append(out, addr)
	}
	return out
}

// ListenerInfo 描述一个运行中的监听器(REST API 用)。
type ListenerInfo struct {
	Address  string `json:"address"`
	Port     int    `json:"port"`
	Protocol string `json:"protocol"`
	Status   string `json:"status"`
}

// ListenerInfos 返回运行中监听器的结构化列表。
func (m *Manager) ListenerInfos() []ListenerInfo {
	m.listenMu.Lock()
	defer m.listenMu.Unlock()
	out := make([]ListenerInfo, 0, len(m.listeners))
	for addr := range m.listeners {
		_, port, err := net.SplitHostPort(addr)
		p := 0
		if err == nil {
			p, _ = strconv.Atoi(port)
		}
		proto := m.listeners[addr].protocol
		if proto == "" {
			proto = "tcp" // 兼容旧 listener(协议字段缺省)
		}
		out = append(out, ListenerInfo{
			Address: addr, Port: p, Protocol: proto, Status: "running",
		})
	}
	return out
}

// AddDefaultListener 用 Manager 默认加密/伪装配置动态启动监听器(REST API 用)。
func (m *Manager) AddDefaultListener(listenAddr string) error {
	return m.AddListener(listenAddr, m.cbox, m.xbox, m.mimic)
}

// Start 启动默认监听器(加密/伪装用 Manager 构造时的配置)并开始心跳清理。
// 兼容旧调用; 动态多点监听用 AddListener。
func (m *Manager) Start(listenAddr string) error {
	if err := m.AddListener(listenAddr, m.cbox, m.xbox, m.mimic); err != nil {
		return err
	}
	// 心跳清理: 每 30s 清理 >30min 离线会话
	go func() {
		defer logx.Guard("cleanup-sweeper") // G3
		ticker := time.NewTicker(30 * time.Second)
		defer ticker.Stop()
		for range ticker.C {
			m.st.CleanupExpired(30 * time.Minute)
		}
	}()
	// 阻塞等待: 默认监听器持续运行
	select {} // Start 不再返回, 由调用方用信号停机
}
