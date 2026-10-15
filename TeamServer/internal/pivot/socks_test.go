package pivot

import (
	"net"
	"testing"
	"time"
)

// TestBuildSocksConnectBytesIPv4 验证多跳透传字节的 IPv4 形式。
func TestBuildSocksConnectBytesIPv4(t *testing.T) {
	b := buildSocksConnectBytes("10.0.0.5", 8080, "", "")
	// 版本协商 05 01 00 + CONNECT 05 01 00 + ATYP 01 + 4B IP + 2B port
	want := []byte{0x05, 0x01, 0x00, 0x05, 0x01, 0x00, 0x01, 10, 0, 0, 5, 0x1f, 0x90}
	for i := range want {
		if b[i] != want[i] {
			t.Fatalf("byte %d: got %#x want %#x (full: %v)", i, b[i], want[i], b)
		}
	}
}

// TestBuildSocksConnectBytesDomain 验证多跳透传字节的域名形式。
func TestBuildSocksConnectBytesDomain(t *testing.T) {
	b := buildSocksConnectBytes("intra", 445, "", "")
	if len(b) != 6+1+1+5+2 {
		t.Fatalf("unexpected length %d: %v", len(b), b)
	}
	if b[6] != 0x03 || b[7] != 5 {
		t.Fatalf("expected domain ATYP+len 03 05, got %v", b[6:8])
	}
	if string(b[8:13]) != "intra" {
		t.Fatalf("domain bytes mismatch: %v", b[8:13])
	}
	if b[13] != 0x01 || b[14] != 0xbd { // 445 = 0x01 0xbd
		t.Fatalf("port mismatch: %v", b[13:15])
	}
}

// TestHandleDataNegotiateAccumulate 验证多跳协商阶段分帧累积: 两段各 6 字节,
// 累积满 12 后把 ack+ok 合法结果投递给 negotiatedCh。
func TestHandleDataNegotiateAccumulate(t *testing.T) {
	client, server := net.Pipe()
	defer func() { _ = client.Close() }()

	pc := &pivotConn{
		pivotID:      "pivot-test",
		conn:         server,
		phase:        "negotiate",
		negotiatedCh: make(chan bool, 1),
	}
	p := &Pivot{pivots: map[string]*pivotConn{"pivot-test": pc}}

	// 第一次只有 ack(2B) + ok 前半截 -> 未满 12, 不投递
	p.handleData(nil, "PivotData", map[string]interface{}{"pivot_id": "pivot-test"}, []byte{0x05, 0x00, 0x05, 0x00, 0x00, 0x01})
	select {
	case ok := <-pc.negotiatedCh:
		t.Fatalf("negotiatedCh 不应在未满 12 字节时被写入, got %v", ok)
	default:
	}
	// 第二次补齐剩余 6 字节(0,0,0,0,0,0) -> 满 12, ack+ok 均合法
	p.handleData(nil, "PivotData", map[string]interface{}{"pivot_id": "pivot-test"},
		[]byte{0, 0, 0, 0, 0, 0})
	select {
	case ok := <-pc.negotiatedCh:
		if !ok {
			t.Fatalf("期望协商成功, 得到失败")
		}
	default:
		t.Fatalf("协商成功应投递 negotiatedCh")
	}
}

// TestHandleDataClosed 验证 pivot 已关闭时不写回 SOCKS 连接。
func TestHandleDataClosed(t *testing.T) {
	client, server := net.Pipe()
	defer func() { _ = client.Close() }()

	pc := &pivotConn{pivotID: "p2", conn: server, closed: true}
	p := &Pivot{pivots: map[string]*pivotConn{"p2": pc}}
	p.handleData(nil, "PivotData", map[string]interface{}{"pivot_id": "p2"}, []byte{1, 2, 3})

	// closed 的 pivot 不写任何字节: 对端在有限时间内读不到数据。
	_ = client.SetReadDeadline(time.Now().Add(50 * time.Millisecond))
	tmp := make([]byte, 16)
	if n, err := client.Read(tmp); err == nil && n > 0 {
		t.Fatalf("closed pivot 不应写回 SOCKS 连接, 写入了 %d 字节", n)
	}
	_ = server.Close()
}

// TestBuildSocksConnectBytesAuth 验证带凭据的透传字节: 方法协商改提 0x02,
// 插入 RFC1929 user/pass 子协商, CONNECT 紧随其后。
func TestBuildSocksConnectBytesAuth(t *testing.T) {
	b := buildSocksConnectBytes("10.0.0.5", 8080, "op", "secret")
	want := []byte{
		0x05, 0x01, 0x02, // 方法协商: 仅提 user/pass
		0x01, 2, 'o', 'p', 6, 's', 'e', 'c', 'r', 'e', 't', // RFC1929 子协商
		0x05, 0x01, 0x00, // CONNECT
		0x01, 10, 0, 0, 5, 0x1f, 0x90, // ATYP=IPv4 + 10.0.0.5:8080
	}
	if len(b) != len(want) {
		t.Fatalf("length %d != %d: %v", len(b), len(want), b)
	}
	for i := range want {
		if b[i] != want[i] {
			t.Fatalf("byte %d: got %#x want %#x (full: %v)", i, b[i], want[i], b)
		}
	}
}

// TestHandleDataNegotiateAuth 验证带鉴权的协商分帧累积: 14 字节
// (ack 05 02 + auth 01 00 + ok) 解析成功。
func TestHandleDataNegotiateAuth(t *testing.T) {
	client, server := net.Pipe()
	defer func() { _ = client.Close() }()

	pc := &pivotConn{
		pivotID:      "pivot-auth",
		conn:         server,
		phase:        "negotiate",
		negotiatedCh: make(chan bool, 1),
	}
	p := &Pivot{pivots: map[string]*pivotConn{"pivot-auth": pc}}

	// 分两帧: 先 ack+auth(4B), 再 ok(10B)
	p.handleData(nil, "PivotData", map[string]interface{}{"pivot_id": "pivot-auth"},
		[]byte{0x05, 0x02, 0x01, 0x00})
	select {
	case ok := <-pc.negotiatedCh:
		t.Fatalf("negotiatedCh 不应在未满 14 字节时被写入, got %v", ok)
	default:
	}
	p.handleData(nil, "PivotData", map[string]interface{}{"pivot_id": "pivot-auth"},
		[]byte{0x05, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0})
	select {
	case ok := <-pc.negotiatedCh:
		if !ok {
			t.Fatalf("期望鉴权协商成功, 得到失败")
		}
	default:
		t.Fatalf("鉴权协商成功应投递 negotiatedCh")
	}
}

// TestHandleDataNegotiateRejectFast 验证入口回 05 FF(无可接受方法)时快速失败,
// 不再等满 12/14 字节。
func TestHandleDataNegotiateRejectFast(t *testing.T) {
	client, server := net.Pipe()
	defer func() { _ = client.Close() }()

	pc := &pivotConn{
		pivotID:      "pivot-rej",
		conn:         server,
		phase:        "negotiate",
		negotiatedCh: make(chan bool, 1),
	}
	p := &Pivot{pivots: map[string]*pivotConn{"pivot-rej": pc}}

	p.handleData(nil, "PivotData", map[string]interface{}{"pivot_id": "pivot-rej"},
		[]byte{0x05, 0xFF})
	select {
	case ok := <-pc.negotiatedCh:
		if ok {
			t.Fatalf("05 FF 应判定协商失败")
		}
	default:
		t.Fatalf("05 FF 应立即投递失败结果")
	}
}
