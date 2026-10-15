// quic.go — QUIC beacon 通道(quic-go): QUIC 连接 → 双向 stream → net.Conn 适配
// → TCP 家族会话循环。
//
// 设计(对齐 ws.go 的桥接模式与既有 D 系列决策):
//     mgr.AcceptConn(protocol.NewQuicConn(...)) 进 runLoop; 推流/心跳/身份复用
//     全部是现成路径, session.Manager 零改动。
//   - 帧协议同 Ws/TCP: [4B 大端长度][Encrypt(frame)][随机填充]; QUIC 自身
//   - ALPN = "cleat-quic"(与 Agent QuicTransport 的 kAlpn 对齐); 证书 = 自签
//     (SelfSignedTLS 复用; QUIC 仅走 TLS1.3, 其 TLS1.2 套件白名单不生效, 无碍)。
//   - deadline 语义(A1-5 的 QUIC 侧): 与 wsNetConn 同款 —— 后台读线程 + 有界
//     队列; 消费侧 deadline 只约束"取队列"等待(到点 net.Error.Timeout 且连接
//     保持可用, runLoop 的"超时 ≠ 断连"原样成立)。读线程**不设 stream
//     deadline**: 闲置由 QUIC 自身 idle timeout 兜底(流错误唤醒读线程退出),
//     不依赖 quic-go 的 deadline 续用语义。
package beacon

import (
	"context"
	"log"
	"net"
	"sync"
	"time"

	"github.com/quic-go/quic-go"

	"cleat/teamserver/internal/protocol"
)

// quicALPN 通道 ALPN(与 Agent QuicTransport 的 kAlpn 对齐)。
const quicALPN = "cleat-quic"

// quicStreamAcceptTimeout 连接建立后等待首条双向 stream 的上限。
// 通道协议: 一条 QUIC 连接承载一条双向 stream(Agent 侧握手完成后立即开流)。
const quicStreamAcceptTimeout = 10 * time.Second

// QUICListener QUIC beacon 监听器(实现 session.QUICHandler 接口)。
type QUICListener struct {
	h    *Handler
	addr string
	ln   *quic.Listener
}

// NewQUICListener 建立 QUIC 监听(UDP; 自签证书 + ALPN=cleat-quic)。
func NewQUICListener(h *Handler, addr string) (*QUICListener, error) {
	tlsConf, err := SelfSignedTLS()
	if err != nil {
		return nil, err
	}
	tlsConf.NextProtos = []string{quicALPN}
	ln, err := quic.ListenAddr(addr, tlsConf, &quic.Config{
		// 4 拍余量口径(与 protocol.ReadTimeout 同源考虑): 空闲连接由 QUIC 自身
		// 超时收摊, 桥接读线程随之唤醒退出。
		MaxIdleTimeout: 120 * time.Second,
	})
	if err != nil {
		return nil, err
	}
	return &QUICListener{h: h, addr: addr, ln: ln}, nil
}

// Serve 接受连接循环(阻塞至 Close)。
func (l *QUICListener) Serve() error {
	for {
		conn, err := l.ln.Accept(context.Background())
		if err != nil {
			return err // Close 后返回; 上层经 done 判定主动停止(静默)
		}
		go l.handleConn(conn)
	}
}

// Close 停止监听(幂等; Accept 循环随即返回)。
func (l *QUICListener) Close() error {
	return l.ln.Close()
}

// handleConn 单条 QUIC 连接的生命周期: 取首条 stream → 桥接 → 会话循环。
func (l *QUICListener) handleConn(conn *quic.Conn) {
	ctx, cancel := context.WithTimeout(context.Background(), quicStreamAcceptTimeout)
	stream, err := conn.AcceptStream(ctx)
	cancel()
	if err != nil {
		log.Printf("[beacon] quic 等待首条 stream 失败 from %s: %v\n", conn.RemoteAddr(), err)
		_ = conn.CloseWithError(0, "")
		return
	}
	nc := &quicNetConn{
		stream: stream,
		closed: make(chan struct{}),
		local:  l.addr,
		remote: conn.RemoteAddr().String(),
	}
	pc := protocol.NewQuicConn(nc, l.h.cbox, l.h.xbox)
	log.Printf("[beacon] quic 连接建立: %s\n", nc.remote)
	// 通道类型 = quic, 随会话写入 transport 列。
	l.h.mgr.AcceptConn(pc, "quic")
	// runLoop 返回 = 连接死亡: 收摊 QUIC 侧
	_ = nc.Close()
	_ = conn.CloseWithError(0, "")
}

// quicNetConn 把 QUIC 双向 stream(*quic.Stream)适配成字节流 net.Conn。
//
// 语义约定(与 wsNetConn 同构):
//   - Read 跨块缓冲: ReadFrame 的 io.ReadFull 两段读横跨同一块剩余字节由
//     buf 承接;
//   - deadline(消费侧): 只约束"取队列"等待 —— 到点返回 net.Error.Timeout 且
//     连接保持可用; 数据由后台读线程继续缓存(A1-5 纪律在 QUIC 侧的等价物)。
type quicNetConn struct {
	stream *quic.Stream

	wmu sync.Mutex // quic-go: 并发写需调用方串行化(task 派发 goroutine 与会话循环并发写帧)

	once       sync.Once
	closedOnce sync.Once
	closed     chan struct{} // Close 后 push 解除阻塞(后台读线程得以退出)
	msgQ       chan qMsg     // 后台读线程 → 消费者(有界; 满即对客户端反压)

	buf  []byte
	rdMu sync.Mutex
	rdl  time.Time // 消费侧读 deadline(zero = 无限; 只约束取队列等待)

	local  string
	remote string
}

// qMsg 后台读线程投递单元(数据块或终态错误)。
type qMsg struct {
	data []byte
	err  error
}

func (c *quicNetConn) Read(p []byte) (int, error) {
	c.once.Do(c.spawnReader)
	for len(c.buf) == 0 {
		var timer *time.Timer
		var timeout <-chan time.Time
		c.rdMu.Lock()
		d := c.rdl
		c.rdMu.Unlock()
		if !d.IsZero() {
			timer = time.NewTimer(time.Until(d))
			timeout = timer.C
		}
		select {
		case m := <-c.msgQ:
			if timer != nil {
				timer.Stop()
			}
			if m.err != nil {
				return 0, m.err
			}
			c.buf = m.data
		case <-timeout:
			// 取队列超时: 连接与缓冲不受影响(数据由后台线程继续缓存),
			// 按标准 net.Error.Timeout 上抛 —— runLoop 视为"暂无数据"继续。
			return 0, errReadTimeout{}
		}
	}
	n := copy(p, c.buf)
	c.buf = c.buf[n:]
	return n, nil
}

// spawnReader 启动后台读线程(每连接一个; stream 错误/关闭后投递终态错误退出)。
// 不设 stream deadline: 闲置由 QUIC idle timeout 兜底(见文件头注)。
func (c *quicNetConn) spawnReader() {
	c.msgQ = make(chan qMsg, 16)
	go func() {
		p := make([]byte, 32*1024)
		for {
			n, err := c.stream.Read(p)
			if n > 0 {
				chunk := make([]byte, n)
				copy(chunk, p[:n])
				if !c.push(qMsg{data: chunk}) {
					return // 消费端已关闭: 退出
				}
			}
			if err != nil {
				c.push(qMsg{err: err})
				return
			}
		}
	}()
}

// push 投递(closed 后返回 false —— 不阻塞在无消费者的队列上, 读线程得以退出)。
func (c *quicNetConn) push(m qMsg) bool {
	select {
	case c.msgQ <- m:
		return true
	case <-c.closed:
		return false
	}
}

func (c *quicNetConn) Write(p []byte) (int, error) {
	c.wmu.Lock()
	defer c.wmu.Unlock()
	return c.stream.Write(p)
}

func (c *quicNetConn) Close() error {
	c.closedOnce.Do(func() { close(c.closed) }) // 解除 push 阻塞
	// 后台读线程可能阻塞在 stream.Read: CancelRead 唤醒之
	c.stream.CancelRead(0)
	c.wmu.Lock()
	defer c.wmu.Unlock()
	return c.stream.Close()
}

// SetReadDeadline 只约束消费侧"取队列"等待(见类型注: deadline 语义)。
func (c *quicNetConn) SetReadDeadline(t time.Time) error {
	c.rdMu.Lock()
	c.rdl = t
	c.rdMu.Unlock()
	return nil
}
func (c *quicNetConn) SetWriteDeadline(t time.Time) error { return c.stream.SetWriteDeadline(t) }
func (c *quicNetConn) SetDeadline(t time.Time) error {
	_ = c.SetReadDeadline(t)
	return c.SetWriteDeadline(t)
}
func (c *quicNetConn) LocalAddr() net.Addr  { return quicAddr{c.local} }
func (c *quicNetConn) RemoteAddr() net.Addr { return quicAddr{c.remote} }

// quicAddr 地址占位实现(网络标记 quic, 字符串即真实地址)。
type quicAddr struct{ addr string }

func (a quicAddr) Network() string { return "quic" }
func (a quicAddr) String() string  { return a.addr }
