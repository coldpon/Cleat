package session

import (
	"io"
	"net"
	"testing"

	"cleat/teamserver/internal/eventbus"
	"cleat/teamserver/internal/protocol"
	"cleat/teamserver/internal/store"
)

// newTestManagerWithSession 构造纯内存 Store + Manager + 一个已注册的在线会话。
// Conn 用 net.Pipe 占位(测试期间不驱动真实帧, 仅验证队列/状态逻辑)。
// client 端由后台 goroutine 持续排干: 节拍合一 S3 后任务直写连接(WriteFrame 同步),
// 无人读的 net.Pipe 会阻塞到写超时。
// 加密用 PSK-only: X25519 模式写帧需先 KE 握手登记 key_id, 测试无握手, 会解不出密钥。
func newTestManagerWithSession(t *testing.T) (*Manager, *Session) {
	st := store.New()
	bus := eventbus.New()
	cbox, _ := testCrypto()
	m := NewManager(st, bus, cbox, nil, false)

	client, server := net.Pipe()
	t.Cleanup(func() { _ = client.Close(); _ = server.Close() })
	go func() { _, _ = io.Copy(io.Discard, client) }()

	sess := New(protocol.NewConn(server, cbox, nil, false), bus)
	sess.Record = &store.Record{ID: 1, RemoteIP: "test", Closed: false}
	m.mu.Lock()
	m.sessions[1] = sess
	m.mu.Unlock()
	return m, sess
}

// TestSubmitTaskOnlineDispatch 在线提交: 任务落库 + 直写连接/beacon outbox + 状态 dispatched。
func TestSubmitTaskOnlineDispatch(t *testing.T) {
	m, _ := newTestManagerWithSession(t)

	id, err := m.SubmitTask(1, "ScreenShot", map[string]interface{}{"format": "png"}, "push")
	if err != nil {
		t.Fatalf("SubmitTask: %v", err)
	}
	if id == 0 {
		t.Fatal("应返回非零任务 ID")
	}

	tk, ok := m.APITask(id)
	if !ok {
		t.Fatal("任务应存在于 store")
	}
	if tk.Status != store.TaskDispatched {
		t.Fatalf("在线任务状态应为 dispatched, got %s", tk.Status)
	}
}

// TestSubmitTaskOfflineThenCompensate 离线提交: 保持 pending; 会话上线(poll)补偿派发。
func TestSubmitTaskOfflineThenCompensate(t *testing.T) {
	cbox, _ := testCrypto()
	m := NewManager(store.New(), eventbus.New(), cbox, nil, false)

	// 会话不存在(离线): 提交后仅落库 pending
	id, err := m.SubmitTask(99, "FileListDir", nil, "pull")
	if err != nil {
		t.Fatalf("SubmitTask: %v", err)
	}
	tk, ok := m.APITask(id)
	if !ok {
		t.Fatal("任务应持久化到 store")
	}
	if tk.Status != store.TaskPending {
		t.Fatalf("离线提交应保持 pending, got %s", tk.Status)
	}

	// 模拟会话上线: 注册一个会话再跑一次 compensate(如同 poll)
	client, server := net.Pipe()
	t.Cleanup(func() { _ = client.Close(); _ = server.Close() })
	go func() { _, _ = io.Copy(io.Discard, client) }()
	sess := New(protocol.NewConn(server, cbox, nil, false), eventbus.New())
	sess.Record = &store.Record{ID: 99, RemoteIP: "x", Closed: false}
	m.mu.Lock()
	m.sessions[99] = sess
	m.mu.Unlock()
	m.compensate(sess)

	if tk2, _ := m.APITask(id); tk2.Status != store.TaskDispatched {
		t.Fatalf("补偿后任务应变为 dispatched, got %s", tk2.Status)
	}
}
