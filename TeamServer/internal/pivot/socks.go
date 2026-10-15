package pivot

// SOCKS5 握手(CONNECT)与 pivot 数据管道。
// 适配自旧 Server/pivot.go: 单跳目标即 SOCKS CONNECT 地址, 多跳透传 CONNECT 字节。

import (
	"encoding/binary"
	"fmt"
	"net"
	"sync"
	"sync/atomic"
	"time"

	"cleat/teamserver/internal/logx"
	"cleat/teamserver/internal/session"
)

// seq 分配唯一 pivotID。
var seq int64

// pivotConn 绑定一个 SOCKS 连接与 Agent 侧的 pivot。
type pivotConn struct {
	sessionID  int
	pivotID    string
	targetHost string
	targetPort int
	conn       net.Conn
	writeMu    sync.Mutex
	closed     bool

	// rportfwd 虚拟连接: 无 SOCKS 客户端, conn 为 Server 拨出的目标;
	// listenPort 用于 Stop 时按监听端口清理在途连接。
	rportfwd   bool
	listenPort int

	// 多跳协商状态: "open"=数据透传; "negotiate"=等待 B 入口的 SOCKS 响应;
	// "rport-pending"=rportfwd 已受理、目标未拨通(缓冲先手数据)
	phase        string
	negotiateBuf []byte // 多跳协商缓冲: 入口 ack(2B) [+ auth(2B)] + ok(10B) 可能分帧/合帧
	pendingBuf   []byte // negotiate 阶段收到的响应后多余字节(响应+数据粘连时) / rportfwd 先手数据
	negotiatedCh chan bool
}

// buildSocksConnectBytes 构造 SOCKS5 CONNECT 请求字节(透传给下一跳入口)。
// user 非空时使用 RFC1929 user/pass 鉴权子协商。
func buildSocksConnectBytes(host string, port uint16, user, pass string) []byte {
	// 发给下一跳入口的是一组完整的 SOCKS5 消息(入口把自己当 SOCKS 服务端,
	// 按序消费):
	//   1. 版本协商 05 01 00 (NO AUTH) 或 05 01 02 (user/pass)
	//   2. user/pass 子协商 01 ulen user plen pass (仅带凭据时)
	//   3. CONNECT 请求 05 01 00 <ATYP> <addr> <port>
	var b []byte
	hasAuth := user != ""
	if hasAuth {
		b = []byte{0x05, 0x01, 0x02}
		// RFC1929 user/pass 子协商(紧随方法协商, 先于 CONNECT)
		b = append(b, 0x01, byte(len(user)))
		b = append(b, user...)
		b = append(b, byte(len(pass)))
		b = append(b, pass...)
	} else {
		b = []byte{0x05, 0x01, 0x00}
	}
	b = append(b, 0x05, 0x01, 0x00)
	if ip := net.ParseIP(host); ip != nil {
		if ip4 := ip.To4(); ip4 != nil {
			b = append(b, 0x01)
			b = append(b, ip4...)
		} else {
			b = append(b, 0x04)
			b = append(b, ip.To16()...)
		}
	} else {
		b = append(b, 0x03, byte(len(host)))
		b = append(b, host...)
	}
	b = append(b, byte(port>>8), byte(port))
	return b
}

// socksFail 向客户端写 SOCKS 拒绝响应并关闭。
func socksFail(conn net.Conn) {
	_, _ = conn.Write([]byte{0x05, 0x04, 0x00, 0x01, 0, 0, 0, 0, 0, 0})
}

// handleSocks SOCKS5 握手(仅 NO AUTH) + CONNECT, 然后建立双向管道。
// entryUser/entryPass 为下一跳入口的 RFC1929 凭据(空 = 入口无鉴权)。
func (p *Pivot) handleSocks(conn net.Conn, sid int, entryHost string, entryPort int,
	entryUser string, entryPass string) {
	defer logx.Guard("pivot-socks-conn") // G3: 每连接一个 goroutine
	defer func() { _ = conn.Close() }()
	_ = conn.SetDeadline(time.Now().Add(30 * time.Second))

	// ---- 1. 版本协商 ----
	buf := make([]byte, 2)
	if _, err := readFull(conn, buf); err != nil || buf[0] != 0x05 {
		return
	}
	methods := make([]byte, buf[1])
	if _, err := readFull(conn, methods); err != nil {
		return
	}
	if _, err := conn.Write([]byte{0x05, 0x00}); err != nil {
		return
	}

	// ---- 2. CONNECT 请求 ----
	hdr := make([]byte, 4)
	if _, err := readFull(conn, hdr); err != nil || hdr[0] != 0x05 {
		return
	}
	if hdr[1] != 0x01 { // 仅支持 CONNECT
		socksFail(conn)
		return
	}
	host, ok := readAtAddr(conn, hdr[3])
	if !ok {
		return
	}
	portBuf := make([]byte, 2)
	if _, err := readFull(conn, portBuf); err != nil {
		return
	}
	port := binary.BigEndian.Uint16(portBuf)

	// ---- 3. 经 Agent 建立连接 ----
	pivotID := fmt.Sprintf("pivot-%d-%d", sid, atomic.AddInt64(&seq, 1))
	pc := &pivotConn{
		sessionID: sid, pivotID: pivotID, conn: conn,
		phase: "open",
	}
	if entryHost != "" {
		pc.targetHost, pc.targetPort = entryHost, entryPort
		pc.phase = "negotiate"
		pc.negotiatedCh = make(chan bool, 1)
	} else {
		pc.targetHost, pc.targetPort = host, int(port)
	}

	resp, err := p.mgr.APICommand(sid, session.CmdPivotStart, map[string]interface{}{
		"pivot_id":    pivotID,
		"target_host": pc.targetHost,
		"target_port": pc.targetPort,
	})
	if err != nil || resp == nil || resp["status"] != "success" {
		socksFail(conn)
		logx.Printf("[pivot] 会话 #%d 连接目标失败 %s:%d\n", sid, pc.targetHost, pc.targetPort)
		return
	}

	p.pivotsMu.Lock()
	p.pivots[pivotID] = pc
	p.pivotsMu.Unlock()
	defer func() {
		p.pivotsMu.Lock()
		delete(p.pivots, pivotID)
		p.pivotsMu.Unlock()
		_ = p.pushClose(pivotID, sid)
	}()

	if entryHost != "" {
		// 多跳: 透传 SOCKS CONNECT 字节给下一跳入口, 等待其成功响应
		logx.Printf("[pivot] 会话 #%d %s 透传 CONNECT %s:%d\n", sid, pivotID, host, int(port))
		if err := p.pushData(pivotID, sid, buildSocksConnectBytes(host, port, entryUser, entryPass)); err != nil {
			logx.Printf("[pivot] 会话 #%d %s 发送 CONNECT 帧失败: %v\n", sid, pivotID, err)
			return
		}
		select {
		case ok := <-pc.negotiatedCh:
			if !ok {
				socksFail(conn)
				logx.Printf("[pivot] 会话 #%d %s 下一跳入口拒绝\n", sid, pivotID)
				return
			}
		case <-time.After(15 * time.Second):
			socksFail(conn)
			logx.Printf("[pivot] 会话 #%d %s 下一跳协商超时\n", sid, pivotID)
			return
		}
		pc.phase = "open"
		pc.negotiatedCh = nil
	}

	// 响应 SOCKS 成功
	if _, err := conn.Write([]byte{0x05, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0}); err != nil {
		return
	}
	_ = conn.SetDeadline(time.Time{})
	logx.Printf("[pivot] 会话 #%d %s -> %s:%d\n", sid, pivotID, host, int(port))

	// ---- 4. SOCKS client -> Agent 管道 ----
	p.relayToAgent(pivotID, sid, conn)
}

// relayToAgent 把 conn 读到的字节作为 PivotData 下推给 Agent; 返回即连接结束。
// socks 出口(SOCKS 客户端方向)与 rportfwd(目标方向)共用。
func (p *Pivot) relayToAgent(pivotID string, sid int, conn net.Conn) {
	buf2 := make([]byte, 64*1024)
	for {
		n, err := conn.Read(buf2)
		if n > 0 {
			if werr := p.pushData(pivotID, sid, buf2[:n]); werr != nil {
				return
			}
		}
		if err != nil {
			return
		}
	}
}

// pushData 把 SOCKS 客户端读到的字节作为 PivotData 混合帧下推给 Agent(非阻塞)。
func (p *Pivot) pushData(pivotID string, sid int, bin []byte) error {
	req := map[string]interface{}{
		"cmd":    session.CmdPivotData,
		"params": map[string]interface{}{"pivot_id": pivotID},
	}
	return p.mgr.APIHybridPush(sid, req, bin)
}

// pushClose 通知 Agent 关闭指定 pivot 出口(混合帧, 非阻塞)。
func (p *Pivot) pushClose(pivotID string, sid int) error {
	req := map[string]interface{}{
		"cmd":    session.CmdPivotClose,
		"params": map[string]interface{}{"pivot_id": pivotID, "reason": "server closed"},
	}
	return p.mgr.APIHybridPush(sid, req, nil)
}

// readFull 读满 b(兼容 net.Conn 读可能不足)。
func readFull(conn net.Conn, b []byte) (int, error) {
	total := 0
	for total < len(b) {
		n, err := conn.Read(b[total:])
		total += n
		if err != nil {
			return total, err
		}
	}
	return total, nil
}

// readAtAddr 读取 SOCKS ATYP 地址段。
func readAtAddr(conn net.Conn, atyp byte) (string, bool) {
	switch atyp {
	case 0x01: // IPv4
		ip := make([]byte, 4)
		if _, err := readFull(conn, ip); err != nil {
			return "", false
		}
		return net.IP(ip).String(), true
	case 0x03: // 域名
		n := make([]byte, 1)
		if _, err := readFull(conn, n); err != nil {
			return "", false
		}
		d := make([]byte, n[0])
		if _, err := readFull(conn, d); err != nil {
			return "", false
		}
		return string(d), true
	case 0x04: // IPv6
		ip := make([]byte, 16)
		if _, err := readFull(conn, ip); err != nil {
			return "", false
		}
		return net.IP(ip).String(), true
	}
	return "", false
}
