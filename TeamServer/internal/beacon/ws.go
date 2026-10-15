// ws.go — WebSocket beacon 通道(wss): 升级 → net.Conn 适配 → TCP 家族会话循环。
//
//     mgr.AcceptConn(protocol.NewWsConn(...)) 进 runLoop; 推流/心跳/身份复用
//     全部是现成路径, session.Manager 零改动。
//     (无 HTTP 伪装壳, WS 升级握手本身即壳); protocol.Conn 以
//     mimic=false + padding=true 剥离。
//     (ReadFrame 的 ReadTimeout=120s = 4 拍余量); 本端不发 WS ping,
//     收到对端 ping 由库按 RFC 自动 pong。
//     自带消息↔字节流适配器。原选型 coder/websocket 的 NetConn 在 deadline 命中时
//     **关闭整条连接**且错误非 net.Error.Timeout(实测: "failed to get reader:
//     context canceled"); gorilla 的 deadline 语义更硬: 读错误(含超时)是**永久态**,
//     超时后继续 NextReader ≡ 自杀(重复读第 1000 次直接 panic, 无任何"重拍"余地)。
//     故适配器自建后台读线程: 帧头等待**不设 deadline**(无限期阻塞, 闲置 ≠ 死亡,
//     连接收摊由 Close 解除阻塞 —— 真·TCP 空闲同构), 仅载荷中途停滞限时判死
//     (详见 wsNetConn 类型注); runLoop 行为与 TCP 完全同构、零改动。
package beacon

import (
	"bytes"
	"io"
	"log"
	"net"
	"net/http"
	"sync"
	"time"

	"github.com/gorilla/websocket"

	"cleat/teamserver/internal/protocol"
)

// WebSocket 返回 wss beacon 处理器。
// 连接被 hijack 后本 handler 常驻至会话断开(等价 TCP listener 的 accept 分支)。
func (h *Handler) WebSocket() http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		up := &websocket.Upgrader{
			// beacon 客户端非浏览器: Origin 校验对其是伪防线, 跳过
			CheckOrigin:       func(*http.Request) bool { return true },
			EnableCompression: false, // 压缩改变消息长度特征(与 D7 填充意图相悖), 关闭
		}
		c, err := up.Upgrade(w, r, nil)
		if err != nil {
			log.Printf("[beacon] ws upgrade 失败 from %s: %v\n", r.RemoteAddr, err)
			return
		}
		// 会话帧上限: 一条 WS 消息 = [4B 长度头][MaxBodySize+填充]
		c.SetReadLimit(protocol.MaxBodySize + 4096)
		// gorilla 的 Conn 非 net.Conn: 消息↔字节流适配 + 真实地址补齐
		// (会话注册/复用按 Conn.RemoteAddr() 入库 rec.RemoteIP)
		nc := &wsNetConn{
			c:      c,
			closed: make(chan struct{}),
			local:  r.Host,
			remote: r.RemoteAddr,
		}
		conn := protocol.NewWsConn(nc, h.cbox, h.xbox)
		log.Printf("[beacon] ws 连接建立: %s %s\n", r.RemoteAddr, r.URL.Path)
		// 通道类型 = ws(wss), 随会话写入 transport 列。
		h.mgr.AcceptConn(conn, "ws")
		// runLoop 返回 = 连接死亡: 收摊 ws 侧
		_ = c.Close()
	}
}

// wsNetConn 把 gorilla 的消息制 Conn 适配成字节流 net.Conn。
//
// 语义约定:
//     Write 一次调用 = 一条消息(WriteFrame 恰好单次 Write/帧);
//   - Read 跨消息缓冲: ReadFrame 的 io.ReadFull(长度头/载荷) 两段读
//     横跨同一消息的剩余字节由 readBuf 承接;
//     "repeated read on failed websocket connection" panic 的教训): 后台读线程
//     帧头等待**不设 deadline** —— 闲置 ≠ 死亡, 无限期阻塞(与 TCP 空闲完全同构),
//     连接收摊由消费侧 Close() 解除阻塞; 载荷中途停滞 = 帧状态不可续, 限时
//     (ReadTimeout)判死关闭(消灭"发几个字节然后挂住"的连接)。消费侧
//     SetReadDeadline 只约束"取队列"等待: 到点返回 net.Error.Timeout 且连接
//     保持可用 —— runLoop 的"超时 ≠ 断连"原样成立。
type wsNetConn struct {
	c *websocket.Conn

	wmu sync.Mutex // gorilla: 并发写需调用方串行化(task 派发 goroutine 与会话循环并发写帧)

	once       sync.Once
	closedOnce sync.Once
	closed     chan struct{} // TS-9: Close 后 push 解除阻塞(后台读线程得以退出)
	msgQ       chan wsMsg    // 后台读线程 → 消费者(有界; 满即对客户端反压)
	buf    []byte     // 当前消息未消费完的剩余字节
	rdMu   sync.Mutex
	rdl    time.Time // 消费侧读 deadline(zero = 无限; 只约束取队列等待)
	local  string
	remote string
}

// wsMsg 后台读线程投递单元(整条消息或终态错误)。
type wsMsg struct {
	data []byte
	err  error
}

func (c *wsNetConn) Read(p []byte) (int, error) {
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
			// 取队列超时: 连接与缓冲不受影响(消息由后台线程继续缓存),
			// 按标准 net.Error.Timeout 上抛 —— runLoop 视为"暂无数据"继续。
			return 0, errReadTimeout{}
		}
	}
	n := copy(p, c.buf)
	c.buf = c.buf[n:]
	return n, nil
}

// errReadTimeout 标准 net.Error 形态的读超时(Timeout()=true, Temporary()=true)。
type errReadTimeout struct{}

func (errReadTimeout) Error() string   { return "ws netconn: read timeout" }
func (errReadTimeout) Timeout() bool   { return true }
func (errReadTimeout) Temporary() bool { return true }

// spawnReader 启动后台读线程(每连接一个; 连接死亡/判死后投递终态错误退出)。
//
// **gorilla 读错误永久律**: readErr 一旦因任何
// 读错误(含 deadline 超时)置位即永久 —— 后续 NextReader 只会立即重复返回同一
// 错误并递增 readErrCount, 第 1000 次直接 panic("repeated read on failed
// websocket connection", conn.go 的防误用机制)。即"超时后重拍"在本库上等价自杀,
// 故帧头等待不设 deadline、不做闲置判定; 连接收摊一律经 Close(直接关闭底层
// net.Conn, 阻塞中的读立即随错返回 —— A1-5 "不会永久阻塞"由此保证)。
// 锚定测试: TestGorillaReadErrIsPermanent。
func (c *wsNetConn) spawnReader() {
	c.msgQ = make(chan wsMsg, 16)
	go func() {
		p := make([]byte, 32*1024)
		for {
			// 帧头等待: 无限期阻塞 —— 闲置 ≠ 死亡(与 TCP 空闲完全同构);
			// 不设 deadline(见函数注: 超时一旦发生读侧即永久死亡, 无重拍余地)。
			typ, r, err := c.c.NextReader()
			if err != nil {
				c.push(wsMsg{err: err})
				return
			}
			if typ != websocket.BinaryMessage {
				// 会话协议只用 binary —— 文本帧 = 协议错, 判死
				c.push(wsMsg{err: errTextFrame})
				_ = c.c.Close()
				return
			}
			// 载荷整体读出: 每段读限时; **中途停滞 = 帧状态不可续, 判死关闭**
			// (消灭"发几个字节然后挂住"的连接 —— A1-5 在桥接层的等价物)
			var msg bytes.Buffer
			for {
				if err := c.c.SetReadDeadline(time.Now().Add(protocol.ReadTimeout)); err != nil {
					c.push(wsMsg{err: err})
					_ = c.c.Close()
					return
				}
				n, rerr := r.Read(p)
				if n > 0 {
					msg.Write(p[:n])
					if msg.Len() > protocol.MaxBodySize+4096 {
						c.push(wsMsg{err: errTooLarge})
						_ = c.c.Close()
						return
					}
				}
				if rerr == io.EOF {
					break // 消息完整
				}
				if rerr != nil {
					c.push(wsMsg{err: rerr}) // 含载荷中途停滞超时: 判死
					_ = c.c.Close()
					return
				}
			}
			c.push(wsMsg{data: msg.Bytes()})
			// 消息收全: 撤下载荷读的 deadline, 帧头等待回到"无限期"语义
			// (不撤则下一条帧头等待会被旧 deadline 到点误杀)。
			if err := c.c.SetReadDeadline(time.Time{}); err != nil {
				c.push(wsMsg{err: err})
				return
			}
		}
	}()
}

func (c *wsNetConn) push(m wsMsg) {
	// TS-9: closed 后不再投递 —— 否则队列满时 push 永久阻塞, 后台读线程泄漏
	// (连接已死但 goroutine 卡在无消费者的队列上)。
	select {
	case c.msgQ <- m:
	case <-c.closed:
	}
}

// errTextFrame / errTooLarge: 桥接层协议错(与 runLoop 的非超时错误同路 → 断连收尾)。
type wsBridgeErr string

func (e wsBridgeErr) Error() string { return string(e) }

const (
	errTextFrame = wsBridgeErr("ws netconn: unexpected text frame")
	errTooLarge  = wsBridgeErr("ws netconn: message too large")
)

func (c *wsNetConn) Write(p []byte) (int, error) {
	c.wmu.Lock()
	defer c.wmu.Unlock()
	if err := c.c.WriteMessage(websocket.BinaryMessage, p); err != nil {
		return 0, err
	}
	return len(p), nil
}

func (c *wsNetConn) Close() error {
	c.closedOnce.Do(func() { close(c.closed) }) // TS-9: 解除 push 阻塞
	c.wmu.Lock()
	defer c.wmu.Unlock()
	return c.c.Close()
}

// SetReadDeadline 只约束消费侧"取队列"等待(见类型注: deadline 语义)。
func (c *wsNetConn) SetReadDeadline(t time.Time) error {
	c.rdMu.Lock()
	c.rdl = t
	c.rdMu.Unlock()
	return nil
}
func (c *wsNetConn) SetWriteDeadline(t time.Time) error { return c.c.SetWriteDeadline(t) }
func (c *wsNetConn) SetDeadline(t time.Time) error {
	_ = c.SetReadDeadline(t)
	return c.SetWriteDeadline(t)
}
func (c *wsNetConn) LocalAddr() net.Addr  { return wsAddr{c.local} }
func (c *wsNetConn) RemoteAddr() net.Addr { return wsAddr{c.remote} }

// wsAddr 地址占位实现(网络标记 wss, 字符串即真实地址)。
type wsAddr struct{ addr string }

func (a wsAddr) Network() string { return "wss" }
func (a wsAddr) String() string  { return a.addr }
