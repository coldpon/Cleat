// quic_test.go — QUIC 通道桥接测试: dial → quicNetConn 适配 → NewQuicConn 帧协议端到端。
// A1-5(deadline 超时返回 net.Error.Timeout 且**连接保持可用** —— runLoop 的
// "超时 ≠ 断连"纪律) / RemoteAddr 补齐(rec.RemoteIP 入库依赖) / Close 解除 push 阻塞。
// 与 ws_test.go 同法: 桥接段在测试内复刻(会话层不在本测试范围)。
package beacon

import (
	"bytes"
	"context"
	"crypto/tls"
	"io"
	"net"
	"testing"
	"time"

	"github.com/quic-go/quic-go"

	"cleat/teamserver/internal/protocol"
)

// startQuicTestPair 起生产参数路径的 QUIC listener(自签 TLS + ALPN + idle 配置),
// 并在测试内复刻 quic.go 的桥接段(逐字同构): 取首条 stream → quicNetConn →
// protocol.NewQuicConn。返回 (服务端 protocol.Conn, 客户端 stream, 清理函数)。
func startQuicTestPair(t *testing.T) (*protocol.Conn, *quic.Stream, func()) {
	t.Helper()
	ql, err := NewQUICListener(nil, "127.0.0.1:0") // h=nil: 不接 mgr(桥接段复刻)
	if err != nil {
		t.Fatalf("NewQUICListener: %v", err)
	}
	addr := ql.ln.Addr().String()

	type pair struct {
		pc     *protocol.Conn
		nc     *quicNetConn
		remote string
	}
	got := make(chan pair, 1)
	go func() {
		conn, err := ql.ln.Accept(context.Background())
		if err != nil {
			return
		}
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		stream, err := conn.AcceptStream(ctx)
		if err != nil {
			_ = conn.CloseWithError(0, "")
			return
		}
		nc := &quicNetConn{
			stream: stream,
			closed: make(chan struct{}),
			local:  addr,
			remote: conn.RemoteAddr().String(),
		}
		got <- pair{pc: protocol.NewQuicConn(nc, nil, nil), nc: nc, remote: conn.RemoteAddr().String()}
	}()

	// 客户端 dial(自签 → 跳过校验; ALPN 与生产对齐)
	cliTLS := &tls.Config{InsecureSkipVerify: true, NextProtos: []string{quicALPN}}
	conn, err := quic.DialAddr(context.Background(), addr, cliTLS, nil)
	if err != nil {
		_ = ql.Close()
		t.Fatalf("DialAddr: %v", err)
	}
	clientStream, err := conn.OpenStreamSync(context.Background())
	if err != nil {
		_ = conn.CloseWithError(0, "")
		_ = ql.Close()
		t.Fatalf("OpenStreamSync: %v", err)
	}
	// kick: quic-go 客户端"开流"是乐观的, 不写数据不发 STREAM 帧 —— 服务端
	// AcceptStream 将等不到(真实 Agent 开流后立即发首帧, 同形唤醒)。
	if _, err := clientStream.Write(framed(t, []byte("kick"))); err != nil {
		_ = conn.CloseWithError(0, "")
		_ = ql.Close()
		t.Fatalf("kick write: %v", err)
	}

	var pr pair
	select {
	case pr = <-got:
	case <-time.After(5 * time.Second):
		t.Fatalf("服务端桥接未就绪")
	}
	if pr.remote == "" {
		t.Fatalf("RemoteAddr 占位未补齐")
	}
	// 消费 kick 帧(唤醒流 accept 的哨兵; 真实 Agent 开流后立即发首帧, 同形)。
	pr.pc.Conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	if _, err := pr.pc.ReadFrame(); err != nil {
		t.Fatalf("消费 kick 帧: %v", err)
	}
	pr.pc.Conn.SetReadDeadline(time.Time{})
	cleanup := func() {
		_ = pr.nc.Close()
		_ = conn.CloseWithError(0, "")
		_ = ql.Close()
	}
	return pr.pc, clientStream, cleanup
}

// TestQuicBridgeFrameRoundTrip 帧协议双向 + deadline 语义(A1-5)。
func TestQuicBridgeFrameRoundTrip(t *testing.T) {
	srvPC, cli, cleanup := startQuicTestPair(t)
	defer cleanup()

	// --- agent → server: [4B][填充载荷] 应被 ReadFrame 剥壳还原 ---
	up := []byte(`{"type":"first_connect"}`)
	if _, err := cli.Write(framed(t, up)); err != nil {
		t.Fatalf("client write: %v", err)
	}
	srvPC.Conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	body, err := srvPC.ReadFrame()
	if err != nil {
		t.Fatalf("ReadFrame: %v", err)
	}
	if !bytes.Equal(body, up) {
		t.Fatalf("上行帧失真: got %q want %q", body, up)
	}

	// --- server → agent: WriteFrame(填充, 无壳) 经适配器原样到达 ---
	down := []byte(`{"type":"command","id":7}`)
	srvPC.Conn.SetWriteDeadline(time.Now().Add(2 * time.Second))
	if err := srvPC.WriteFrame(down); err != nil {
		t.Fatalf("WriteFrame: %v", err)
	}
	wire := readQuicWire(t, cli)
	if got := unframe(t, wire); !bytes.Equal(got, down) {
		t.Fatalf("下行帧失真: got %q want %q", got, down)
	}
}

// TestQuicBridgeDeadlineTimeoutKeepsConnUsable deadline 超时后连接必须仍可用
// (runLoop 的"超时 ≠ 断连"纪律; 注: 不直接对 ReadFrame 设短 deadline —— 它入口
// 会自设 protocol.ReadTimeout, 故经裸 nc.Read 验证桥接层语义)。
func TestQuicBridgeDeadlineTimeoutKeepsConnUsable(t *testing.T) {
	srvPC, cli, cleanup := startQuicTestPair(t)
	defer cleanup()

	srvPC.Conn.SetReadDeadline(time.Now().Add(50 * time.Millisecond))
	start := time.Now()
	small := make([]byte, 16)
	_, err := srvPC.Conn.Read(small)
	if err == nil {
		t.Fatalf("期望超时错误")
	}
	if d := time.Since(start); d > time.Second {
		t.Fatalf("超时未按 deadline 触发: %v", d)
	}
	if ne, ok := err.(net.Error); !ok || !ne.Timeout() {
		t.Fatalf("超时错误应为 net.Error.Timeout: %v", err)
	}

	// 超时后连接必须仍然可用: 清 deadline 再走一帧完整往返
	srvPC.Conn.SetReadDeadline(time.Time{})
	if _, err := cli.Write(framed(t, []byte("late"))); err != nil {
		t.Fatalf("超时后 client write: %v", err)
	}
	srvPC.Conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	body, err := srvPC.ReadFrame()
	if err != nil {
		t.Fatalf("超时后 ReadFrame(连接应存活): %v", err)
	}
	if !bytes.Equal(body, []byte("late")) {
		t.Fatalf("超时后上行帧失真: %q", body)
	}
}

// TestQuicNetConnCloseUnblocksPush Close 解除 push 阻塞 —— 否则队列满 + 连接
// 死亡时后台读线程永久卡在无消费者的队列上(goroutine 泄漏, wsNetConn TS-9 同款)。
func TestQuicNetConnCloseUnblocksPush(t *testing.T) {
	_, _, cleanup := startQuicTestPair(t)
	defer cleanup()

	// 单独构造一个桥接实例(不依赖上面那对): 用一个已建立的真实 stream
	ql, err := NewQUICListener(nil, "127.0.0.1:0")
	if err != nil {
		t.Fatalf("NewQUICListener: %v", err)
	}
	defer ql.Close()
	addr := ql.ln.Addr().String()

	got := make(chan *quicNetConn, 1)
	go func() {
		conn, err := ql.ln.Accept(context.Background())
		if err != nil {
			return
		}
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		stream, err := conn.AcceptStream(ctx)
		if err != nil {
			return
		}
		nc := &quicNetConn{
			stream: stream,
			closed: make(chan struct{}),
			local:  addr,
			remote: conn.RemoteAddr().String(),
		}
		got <- nc
	}()

	cliTLS := &tls.Config{InsecureSkipVerify: true, NextProtos: []string{quicALPN}}
	conn, err := quic.DialAddr(context.Background(), addr, cliTLS, nil)
	if err != nil {
		t.Fatalf("DialAddr: %v", err)
	}
	defer func() { _ = conn.CloseWithError(0, "") }()
	cliStream, err := conn.OpenStreamSync(context.Background())
	if err != nil {
		t.Fatalf("OpenStreamSync: %v", err)
	}
	// kick: 同 startQuicTestPair(唤醒服务端 AcceptStream)
	if _, err := cliStream.Write(framed(t, []byte("kick"))); err != nil {
		t.Fatalf("kick write: %v", err)
	}

	var nc *quicNetConn
	select {
	case nc = <-got:
	case <-time.After(5 * time.Second):
		t.Fatalf("桥接未就绪")
	}
	nc.spawnReader() // 显式启动后台读线程(Read 侧 lazy 启动, 这里直接驱动)

	// 等 kick 被读线程读入队列并丢弃(队列容量 16, 需精确测"满队列 push 阻塞")
	kickDeadline := time.Now().Add(2 * time.Second)
	for len(nc.msgQ) == 0 {
		if time.Now().After(kickDeadline) {
			t.Fatalf("kick 未到达读线程队列")
		}
		time.Sleep(10 * time.Millisecond)
	}
	<-nc.msgQ

	// 灌满 msgQ(16) —— 绕过真实网络直接塞队列
	for i := 0; i < 16; i++ {
		nc.msgQ <- qMsg{data: []byte("x")}
	}
	// push 应阻塞(队列满)
	pushed := make(chan struct{})
	go func() {
		nc.push(qMsg{data: []byte("y")})
		close(pushed)
	}()
	select {
	case <-pushed:
		t.Fatal("满队列 push 不应立即返回")
	case <-time.After(150 * time.Millisecond):
	}

	// Close → closed 关闭 → push 解除阻塞(后台线程随后退出, 不泄漏)
	_ = nc.Close()
	select {
	case <-pushed:
	case <-time.After(2 * time.Second):
		t.Fatal("Close 应解除 push 阻塞")
	}
}

// readQuicWire 从 stream 读一条完整线上帧([4B 头 + 载荷])。
func readQuicWire(t *testing.T, s *quic.Stream) []byte {
	t.Helper()
	hdr := make([]byte, 4)
	if _, err := io.ReadFull(s, hdr); err != nil {
		t.Fatalf("读长度头: %v", err)
	}
	n := uint32(hdr[0])<<24 | uint32(hdr[1])<<16 | uint32(hdr[2])<<8 | uint32(hdr[3])
	buf := make([]byte, n)
	if _, err := io.ReadFull(s, buf); err != nil {
		t.Fatalf("读载荷: %v", err)
	}
	return append(hdr, buf...)
}
