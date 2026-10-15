// ws_test.go — WebSocket 通道桥接测试: 升级 → wsNetConn 适配 → NewWsConn 帧协议端到端。
// A1-5(deadline 超时返回 net.Error.Timeout 且**连接保持可用** —— runLoop 的
// "超时 ≠ 断连"纪律) / 真实 RemoteAddr 补齐(rec.RemoteIP 入库依赖) /
// gorilla 读错误永久律锚定。
package beacon

import (
	"bytes"
	"encoding/binary"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/gorilla/websocket"

	"cleat/teamserver/internal/protocol"
)

// (格式与 protocol 包内 addFramePadding 一致; 测试侧独立复刻, 不导出生产符号。)
func padFrame(t *testing.T, body []byte) []byte {
	t.Helper()
	const padLen = 37 // 固定值: 断言可重现(剥填充后还原 body 即可)
	out := make([]byte, 0, len(body)+padLen+2)
	out = append(out, body...)
	for i := 0; i < padLen; i++ {
		out = append(out, 0xA5)
	}
	out = append(out, byte(padLen), byte(padLen>>8))
	return out
}

// framed 将会话载荷封成 [4B 大端长度][加填充载荷](agent 发送形状)。
func framed(t *testing.T, body []byte) []byte {
	t.Helper()
	padded := padFrame(t, body)
	out := make([]byte, 4, 4+len(padded))
	binary.BigEndian.PutUint32(out[:4], uint32(len(padded)))
	out = append(out, padded...)
	return out
}

// unframe 剥 [4B 长度头] + 填充, 还原服务端 WriteFrame 后的载荷对称物。
func unframe(t *testing.T, wire []byte) []byte {
	t.Helper()
	if len(wire) < 6 {
		t.Fatalf("wire 太短: %d", len(wire))
	}
	declared := binary.BigEndian.Uint32(wire[:4])
	if int(declared) != len(wire)-4 {
		t.Fatalf("长度头不一致: declared=%d actual=%d", declared, len(wire)-4)
	}
	body := wire[4:]
	padLen := int(body[len(body)-2]) | int(body[len(body)-1])<<8
	return body[:len(body)-padLen-2]
}

func TestWsBridgeFrameRoundTrip(t *testing.T) {
	type gotConn struct {
		pc     *protocol.Conn
		nc     *wsNetConn
		remote string
	}
	got := make(chan gotConn, 1)

	// mgr=nil 不能跑 AcceptConn(会话层属 session 包, 不在本测试范围);
	// 桥接正确性直接在测试内复刻 ws.go 的桥接段(逐字同构), 断言帧协议。
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		up := &websocket.Upgrader{
			CheckOrigin:       func(*http.Request) bool { return true },
			EnableCompression: false,
		}
		c, err := up.Upgrade(w, r, nil)
		if err != nil {
			t.Errorf("accept: %v", err)
			return
		}
		c.SetReadLimit(protocol.MaxBodySize + 4096)
		nc := &wsNetConn{c: c, local: r.Host, remote: r.RemoteAddr}
		pc := protocol.NewWsConn(nc, nil, nil)
		got <- gotConn{pc: pc, nc: nc, remote: r.RemoteAddr}
		<-r.Context().Done()
	}))
	defer srv.Close()

	wsURL := "ws" + strings.TrimPrefix(srv.URL, "http")
	client, _, err := websocket.DefaultDialer.Dial(wsURL, nil)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	defer client.Close()

	gc := <-got
	if gc.remote == "" || strings.HasPrefix(gc.remote, "unknown") {
		t.Fatalf("RemoteAddr 占位未补齐: %q", gc.remote)
	}

	// --- agent → server: [4B][填充载荷] 应被 ReadFrame 剥壳还原 ---
	up := []byte(`{"type":"first_connect"}`)
	if err := client.WriteMessage(websocket.BinaryMessage, framed(t, up)); err != nil {
		t.Fatalf("client write: %v", err)
	}
	gc.pc.Conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	body, err := gc.pc.ReadFrame()
	if err != nil {
		t.Fatalf("ReadFrame: %v", err)
	}
	if !bytes.Equal(body, up) {
		t.Fatalf("上行帧失真: got %q want %q", body, up)
	}

	// --- server → agent: WriteFrame(填充, 无壳) 经适配器原样到达 ---
	down := []byte(`{"type":"command","id":7}`)
	gc.pc.Conn.SetWriteDeadline(time.Now().Add(2 * time.Second))
	if err := gc.pc.WriteFrame(down); err != nil {
		t.Fatalf("WriteFrame: %v", err)
	}
	typ, raw, err := client.ReadMessage()
	if err != nil {
		t.Fatalf("client read: %v", err)
	}
	if typ != websocket.BinaryMessage {
		t.Fatalf("帧类型: %v", typ)
	}
	if got2 := unframe(t, raw); !bytes.Equal(got2, down) {
		t.Fatalf("下行帧失真: got %q want %q", got2, down)
	}

	// --- deadline 语义(A1-5, 选型的核心判据): 超时返回 net.Error.Timeout,
	// 且**连接保持可用** —— runLoop 的"超时 ≠ 断连"纪律原样成立。
	// (注: 不能对 ReadFrame 设短 deadline, 它入口会自设 ReadTimeout=120s。)
	gc.nc.SetReadDeadline(time.Now().Add(50 * time.Millisecond))
	start := time.Now()
	small := make([]byte, 16)
	_, err = gc.nc.Read(small)
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
	gc.nc.SetReadDeadline(time.Time{})
	if err := client.WriteMessage(websocket.BinaryMessage, framed(t, up)); err != nil {
		t.Fatalf("超时后 client write: %v", err)
	}
	gc.pc.Conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	body2, err := gc.pc.ReadFrame()
	if err != nil {
		t.Fatalf("超时后 ReadFrame(连接应存活): %v", err)
	}
	if !bytes.Equal(body2, up) {
		t.Fatalf("超时后上行帧失真: got %q want %q", body2, up)
	}
}

// TS-9: Close 解除 push 阻塞 —— 否则队列满 + 连接死亡时后台读线程永久卡在
// 无消费者的队列上(goroutine 泄漏)。
func TestWsNetConnCloseUnblocksPush(t *testing.T) {
	got := make(chan *wsNetConn, 1)
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		up := &websocket.Upgrader{CheckOrigin: func(*http.Request) bool { return true }}
		c, err := up.Upgrade(w, r, nil)
		if err != nil {
			return
		}
		c.SetReadLimit(protocol.MaxBodySize + 4096)
		nc := &wsNetConn{c: c, closed: make(chan struct{}), local: r.Host, remote: r.RemoteAddr}
		got <- nc
		<-r.Context().Done()
	}))
	defer srv.Close()

	wsURL := "ws" + strings.TrimPrefix(srv.URL, "http")
	client, _, err := websocket.DefaultDialer.Dial(wsURL, nil)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}

	nc := <-got
	nc.spawnReader() // 显式启动后台读线程(Read 侧 lazy 启动, 这里直接驱动)

	// 灌满 msgQ(16) —— 绕过真实网络直接塞队列
	for i := 0; i < 16; i++ {
		nc.msgQ <- wsMsg{data: []byte("x")}
	}
	// push 应阻塞(队列满)
	pushed := make(chan struct{})
	go func() {
		nc.push(wsMsg{data: []byte("y")})
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
		t.Fatal("Close 应解除 push 阻塞(TS-9)")
	}
	_ = client.Close()
}

// TestGorillaReadErrIsPermanent — 上游事实锚定:
// gorilla 的读错误是**永久态**。读 deadline 到点后, NextReader 之后每次调用都
// **立即**重复返回同一错误并递增内部 readErrCount(防误用计数), 第 1000 次直接
// panic("repeated read on failed websocket connection")。
// 即: "读超时 → continue 重拍"在本库上等价自杀 —— 帧头等待因此不设 deadline
// (见 ws.go spawnReader 注)。若上游未来改变该行为(不再 panic), 请重新评估
// 重拍方案, 不要默默删除本测试。
func TestGorillaReadErrIsPermanent(t *testing.T) {
	connCh := make(chan *websocket.Conn, 1)
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		up := &websocket.Upgrader{CheckOrigin: func(*http.Request) bool { return true }}
		c, err := up.Upgrade(w, r, nil)
		if err != nil {
			return
		}
		connCh <- c
		<-r.Context().Done()
	}))
	defer srv.Close()

	wsURL := "ws" + strings.TrimPrefix(srv.URL, "http")
	client, _, err := websocket.DefaultDialer.Dial(wsURL, nil)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	defer client.Close()
	sc := <-connCh
	defer sc.Close()

	// 第 1 次: 闲置到 deadline 到点 → net.Error.Timeout(readErr 就此永久置位)
	if err := sc.SetReadDeadline(time.Now().Add(30 * time.Millisecond)); err != nil {
		t.Fatalf("set deadline: %v", err)
	}
	_, _, err = sc.NextReader()
	if ne, ok := err.(net.Error); !ok || !ne.Timeout() {
		t.Fatalf("首次超时应为 net.Error.Timeout: %v", err)
	}

	// 重拍: 每次调用应立即返回同一错误; 第 1000 次 panic(即"超时后重拍"必死)
	defer func() {
		if r := recover(); r == nil {
			t.Fatal("第 1000 次重复读未 panic(上游行为已变, 见测试注)")
		} else {
			t.Logf("上游 panic 已复现(预期): %v", r)
		}
	}()
	for i := 2; i <= 1100; i++ {
		if _, _, err := sc.NextReader(); err == nil {
			t.Fatalf("第 %d 次调用应返回同一错误", i)
		}
	}
}
