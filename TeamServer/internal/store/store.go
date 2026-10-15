// Package store 会话存储层: 会话与 machine_id 映射的单一所有权点。
// 业界惯例: 存储层只持有数据与并发安全操作, 不含连接/协议业务逻辑(后者在 session 包)。
// 后续落盘(SQLite)/分布式可用本包替换实现, session 包无需改动。
package store

import (
	"sync"
	"time"

	"cleat/teamserver/internal/logx"
)

// Record 是一个会话的持久状态(连接/协议逻辑在 Session 持有并操作其指针)。
// 字段对齐旧 Server Session 状态机所需的最小集, 后续按需扩展。
type Record struct {
	ID        int
	MachineID string

	// Info 主机信息(由 FirstConnect 填充)
	Hostname  string
	OS        string
	OSVersion string
	CPU       string
	Memory    string
	LocalIP   string
	Arch      string

	// RemoteIP 当前连接远端地址
	RemoteIP string

	// Transport 会话通道类型: tcp | https | http | dns | ws | pipe。
	// 由各连接入口在会话建立时写入(AcceptConn / NewBeaconSession / 复活路径);
	// 空值 = 未知(Web 侧显示占位, 不再伪造 'tcp')。
	Transport string

	// 进程信息(由 FirstConnect 填充)
	PID         uint32
	ProcessName string
	Privilege   string // SYSTEM / High / Medium / Low / Unknown

	// BuildID 编译期构建标识(FirstConnect 上报; 会话接管三元校验之一, 见注入与迁移设计)
	BuildID string

	// ElevationInfo 自提权: 该会话被提升实例替换的时间(零值=未替换)
	ElevatedReplacedAt time.Time

	// infoMu 保护下列可变状态(LastSeen/Closed)
	infoMu   sync.Mutex
	LastSeen time.Time
	Closed   bool
}

// Lock/Unlock infoMu 的便捷方法(Manager 状态机在防抖/复用判断时使用)。
func (r *Record) Lock()   { r.infoMu.Lock() }
func (r *Record) Unlock() { r.infoMu.Unlock() }

// IsClosed/SetClosed Closed 字段的收口读写(TS-4/G7): 该字段曾被 Session.mu(s.mu)/
// infoMu/无锁 三路并发读写(-race 实锤: manager.go:319/321 vs sendFrame:81), 全部收口经 infoMu。
func (r *Record) IsClosed() bool {
	r.infoMu.Lock()
	defer r.infoMu.Unlock()
	return r.Closed
}

func (r *Record) SetClosed(v bool) {
	r.infoMu.Lock()
	r.Closed = v
	r.infoMu.Unlock()
}

// Store 是会话存储。
type Store struct {
	mu       sync.RWMutex
	sessions map[int]*Record
	machine  map[string]int // machine_id -> session_id
	nextID   int
	persist  Persistence // nil=纯内存

	// 任务表(内存态权威, SQLite 为落盘镜像)
	taskMu     sync.Mutex
	tasks      map[int64]*Task
	nextTaskID int64

	// 操作员表(内存态权威, SQLite 为落盘镜像)
	opMu      sync.Mutex
	operators map[string]*Operator

	// 凭据表(内存态权威, SQLite 为落盘镜像)
	credMu      sync.Mutex
	credentials []Credential
	nextCredID  int64

	// 目标表(内存态权威, SQLite 为落盘镜像)
	targetMu     sync.Mutex
	targets      []Target
	nextTargetID int64
}

// New 创建空存储。
func New() *Store {
	return &Store{
		sessions:  make(map[int]*Record),
		machine:   make(map[string]int),
		nextID:    1, // 0 在 Agent 侧语义为"未注册"
		tasks:     make(map[int64]*Task),
		operators: make(map[string]*Operator),
	}
}

// NewID 分配一个新会话 ID。
func (s *Store) NewID() int {
	s.mu.Lock()
	defer s.mu.Unlock()
	id := s.nextID
	s.nextID++
	return id
}

// Put 加入或更新一个会话(记录 machine_id 映射 + 持久化)。
func (s *Store) Put(r *Record) {
	s.mu.Lock()
	s.sessions[r.ID] = r
	if r.MachineID != "" {
		s.machine[r.MachineID] = r.ID
	}
	persist := s.persist
	s.mu.Unlock()
	if persist != nil {
		_ = persist.SaveSession(r)
	}
}

// Get 按 id 取会话(可能已离线)。
func (s *Store) Get(id int) *Record {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.sessions[id]
}

// ByMachine 按 machine_id 取会话(用于重连复用), 返回 nil 如果不存在。
func (s *Store) ByMachine(machineID string) *Record {
	s.mu.RLock()
	defer s.mu.RUnlock()
	id, ok := s.machine[machineID]
	if !ok {
		return nil
	}
	return s.sessions[id]
}

// Remove 删除会话(仅用于彻底清除; 一般离线不清, 保留供重连复用)。
func (s *Store) Remove(id int) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	r, ok := s.sessions[id]
	if !ok {
		return false
	}
	if r.MachineID != "" {
		delete(s.machine, r.MachineID)
	}
	delete(s.sessions, id)
	return true
}

// All 返回所有会话的快照。
func (s *Store) All() []*Record {
	s.mu.RLock()
	defer s.mu.RUnlock()
	out := make([]*Record, 0, len(s.sessions))
	for _, r := range s.sessions {
		out = append(out, r)
	}
	return out
}

// CleanupExpired 清理长时间离线(>maxAge 无心跳)的会话, 防止会话表膨胀。
// 内存与持久化同步删除: 只删内存不删 SQLite 会导致历史记录每次启动
// 重新加载并被重复"删除"(cleanup 日志刷屏)。
// 返回清理数量。
func (s *Store) CleanupExpired(maxAge time.Duration) int {
	cutoff := time.Now().Add(-maxAge)
	removed := 0
	var toDelete []int
	s.mu.Lock()
	for id, r := range s.sessions {
		if r.Closed && r.LastSeen.Before(cutoff) {
			// R9 观测点: 实测在线会话记录在 ~2h 断连后 ByMachine 落空(被迫
			// brandNew 新 sid), 而 4min 间隔重连却能复用 —— 打印每次删除的
			// id/LastSeen 以定位是哪条记录、何时被清(是否误删)
			logx.Printf("[cleanup] 删除离线会话记录 #%d (machine=%s last_seen=%s)\n",
				id, r.MachineID, r.LastSeen.Format("01-02 15:04:05"))
			if r.MachineID != "" {
				delete(s.machine, r.MachineID)
			}
			delete(s.sessions, id)
			toDelete = append(toDelete, id)
			removed++
		}
	}
	persist := s.persist
	s.mu.Unlock()
	// 同步删除持久化记录(锁外执行, 避免 SQLite I/O 占用会话锁)
	if persist != nil {
		for _, id := range toDelete {
			if err := persist.DeleteSession(id); err != nil {
				logx.Printf("[cleanup] 删除会话 #%d 持久化记录失败: %v\n", id, err)
			}
		}
	}
	return removed
}
