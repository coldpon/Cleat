// Package session 会话生命周期: 单个 Agent 连接的协议读取、命令双通道、注册/复用状态机。
// 状态数据落在 store.Record, 连接/写入/锁在 session 包; 业务推送在 handlers 包。
package session

import (
	"fmt"
	"runtime"
	"sync"
	"time"

	"cleat/teamserver/internal/eventbus"
	"cleat/teamserver/internal/protocol"
	"cleat/teamserver/internal/store"
)

const (
	// HeartbeatTimeout 心跳超时阈值: Agent 每 ~30s 心跳, 90s 无心跳视为离线。
	HeartbeatTimeout = 90 * time.Second
)

// reRegisterDebounce 复用注册防抖窗(节拍合一 S4): "旧记录仍活跃"的判定窗。
// 取 2×Agent 空闲节拍 base(默认 60s): beacon 的 LastSeen 随每次 GET/POST 更新,
// 更小的窗在两拍之间必然过期形同虚设; 旧实例停拍后新实例最迟 2 分钟可接管。
// var 而非 const: 持续拒绝死锁破除的单测需要毫秒级窗口;
// 仅测试改写并 defer 恢复, 生产代码禁止改写(与 RespTimeout 同纪律)。
var reRegisterDebounce = 2 * time.Minute

// RespTimeout 命令响应等待上限(push/pull 均适用)。
// (3×RespTimeout=180s 无法在测试里等)。仅测试改写并 defer 恢复; 生产代码禁止改写。
//
// 60s → 300s: 该常量按"活跃会话秒级响应"时代设计,
// 但睡眠掩码部署下(窗长=心跳节拍, cap 120s)命令落在窗口期会被"冻"到出窗才取件执行
// —— 实测最坏端到端 244s(120s 顶格窗 + 让位 + 取件节拍), 60s 上限会造成
// "服务端报超时、命令实际已执行"的误报(task 模式无此问题, 同步 /command 才有)。
// 300s = 实测最坏 244s × 1.23 余量; 超时文案已注明"命令可能仍将执行"。
var RespTimeout = 300 * time.Second

// Session 代表一个植入物连接。
type Session struct {
	// Record 是持久状态(存储层), 指针与 store 共享; LastSeen/Closed 由 Record.infoMu 保护。
	Record *store.Record

	// Conn 帧协议连接(复用旧会话时会被更新为新连接)。
	// beacon 模式(HTTPS/DNS/ICMP, 无长连接)下为 nil, 出站帧走 outbox 由 GET/请求捎带走。
	Conn *protocol.Conn

	// beacon 通道出站帧(待 Agent 拉取的命令帧 FIFO, 顺序保证)。
	beacon   bool
	outbox   [][]byte
	outboxMu sync.Mutex

	// 并发控制
	mu      sync.Mutex // 保护 Conn
	writeMu sync.Mutex // 保护写帧/outbox
	cmdMu   sync.Mutex // 命令互斥: 同一时刻仅一个在途命令

	// 派发互斥: compensate 的并发调用方(BeaconGet/handlePoll/
	// SubmitTask)各拿同一 PendingTasks 快照会**重复派发同一任务**——派发循环整体持
	// 本锁串行化; 后一轮的快照在前一轮全部置 Dispatched 后不再命中 pending。
	// **重构警告**: compensate 循环必须持 dispatchMu, 谁裸调 dispatchTask 谁引入双执行。
	dispatchMu sync.Mutex

	// 命令响应: 唯一语义 = request_id → PendingResp(节拍合一 S3.5;
	// 原 push 侧 RespChan FIFO 匹配已删, 在 Agent 并发执行/响应乱序时会串话)
	// 与 PendingResp 同一把 pendingMu 保护 —— 注册/摘除/取消原子, 防半取消状态。
	// 注意: 状态正确性**不依赖**本表(丢失时 CAS 兜底), 它只影响取消的及时性。
	pendingMu   sync.Mutex
	PendingResp map[string]chan []byte
	taskWaits   map[string]chan struct{}

	// 事件总线(Manager 注入)
	Bus *eventbus.Hub

	// 回调(Manager 设置)
	OnOnline  func(*Session)
	OnOffline func(*Session)
}

// New 创建一个新的连接会话(尚未注册身份, Record.ID 待分配)。
func New(conn *protocol.Conn, bus *eventbus.Hub) *Session {
	return &Session{
		Record:      &store.Record{ID: 0, RemoteIP: "pending"},
		Conn:        conn,
		PendingResp: make(map[string]chan []byte),
		taskWaits:   make(map[string]chan struct{}),
		Bus:         bus,
	}
}

// ID 返回会话 ID。
func (s *Session) ID() int { return s.Record.ID }

// MarkOffline 标记会话离线(保留在 store 供重连复用), 通知 OnOffline。
func (s *Session) MarkOffline() {
	if _, file, line, ok := runtime.Caller(1); ok {
		fmt.Printf("[MarkOffline] caller=%s:%d\n", file, line)
	}
	s.Record.Lock()
	if s.Record.Closed {
		s.Record.Unlock()
		return
	}
	s.Record.Closed = true
	s.Record.Unlock()
	s.mu.Lock()
	if s.Conn != nil {
		s.Conn.Close()
	}
	s.mu.Unlock()
	// 清空在途等待者的注册: **绝不 close(ch)** ——
	// handleOne 的响应投递在锁外 select-default 发送, close 与该发送并发即
	// "send on closed channel" panic(整进程崩)。
	// 代价: 断开瞬间在途命令的等待者靠 RespTimeout 兜底退出(断开罕见, 可接受)。
	// **重构警告(TS 将重写)**: PendingResp 的 channel 生命周期规则 = "只 create/delete,
	// 永不 close"; 谁在这里加回 close, 谁就重新引入全进程崩溃点。
	s.pendingMu.Lock()
	s.PendingResp = make(map[string]chan []byte)
	s.taskWaits = make(map[string]chan struct{})
	s.pendingMu.Unlock()
	if s.OnOffline != nil {
		s.OnOffline(s)
	}
	if s.Bus != nil {
		s.Bus.PublishGlobal(eventbus.NewEvent(eventbus.EventSessionOffline, s.ID(), s.Info()))
	}
}

// Touch 更新心跳时间。
func (s *Session) Touch() {
	s.Record.Lock()
	s.Record.LastSeen = time.Now()
	s.Record.Unlock()
}
