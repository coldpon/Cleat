package session

import (
	"net"
	"testing"
	"time"
)

// fakeDNSHandler 记录 Serve 的进入与退出(底层 conn 关闭即退出)。
type fakeDNSHandler struct {
	serving chan struct{}
	exited  chan struct{}
}

func (f *fakeDNSHandler) Serve(conn *net.UDPConn) error {
	close(f.serving)
	buf := make([]byte, 64)
	for {
		if _, _, err := conn.ReadFromUDP(buf); err != nil {
			close(f.exited)
			return err
		}
	}
}

// TestAddDNSListenerLifecycle 动态 DNS listener: 启动后可列举(协议=dns)、重复添加幂等、
// 删除后 Serve 随 UDP conn 关闭而退出。
func TestAddDNSListenerLifecycle(t *testing.T) {
	m := newTestManager()
	h := &fakeDNSHandler{serving: make(chan struct{}), exited: make(chan struct{})}

	if err := m.AddDNSListener("127.0.0.1:0", h); err != nil {
		t.Fatalf("AddDNSListener: %v", err)
	}
	select {
	case <-h.serving:
	case <-time.After(2 * time.Second):
		t.Fatal("Serve 未被启动")
	}

	// 幂等: 同一地址重复添加不报错(且不会二次启动 Serve)
	if err := m.AddDNSListener("127.0.0.1:0", h); err != nil {
		t.Fatalf("重复 AddDNSListener 应幂等: %v", err)
	}

	found := false
	for _, in := range m.ListenerInfos() {
		if in.Address == "127.0.0.1:0" {
			if in.Protocol != "dns" {
				t.Fatalf("协议应为 dns, got %q", in.Protocol)
			}
			found = true
		}
	}
	if !found {
		t.Fatalf("ListenerInfos 应含 DNS listener: %v", m.ListenerInfos())
	}

	if err := m.RemoveListener("127.0.0.1:0"); err != nil {
		t.Fatalf("RemoveListener: %v", err)
	}
	select {
	case <-h.exited:
	case <-time.After(2 * time.Second):
		t.Fatal("UDP conn 关闭后 Serve 应退出")
	}
	if len(m.ListenerInfos()) != 0 {
		t.Fatalf("删除后不应残留 listener: %v", m.ListenerInfos())
	}
}
