// 任务持久化: 操作员命令的任务表 + 状态机。
// 业界 C2(CS/Sliver)惯例: 任务先落库再派发, Agent 离线不丢, 上线自动补偿。
// 分层: store 只持有数据与并发安全操作; 任务编排(派发/补偿)在 session 包。
package store

import (
	"time"
)

// TaskStatus 任务状态机。
//
//	pending     已入队待派发(Agent 离线时停留)
//	dispatched  已派发等待 Agent 响应
//	completed   Agent 响应成功
//	failed      Agent 响应错误
//	timeout     等待响应超时(可手动重试)。终态 —— 第 3 批接边:
//	            3x 窗耗尽且帧已交付 / SweepStuckTasks 保险丝。此前零引用(定义了没人写),
//	            dispatched 曾永久卡死无终态。
//	            同一命令时旧任务让位。终态 —— 不参与补偿, 晚到响应不复活。
type TaskStatus string

const (
	TaskPending    TaskStatus = "pending"
	TaskDispatched TaskStatus = "dispatched"
	TaskCompleted  TaskStatus = "completed"
	TaskFailed     TaskStatus = "failed"
	TaskTimeout    TaskStatus = "timeout"
	TaskSuperseded TaskStatus = "superseded"
)

// Task 是一条持久化命令任务。
// Params 存 JSON 字符串(避免 store 依赖具体类型); session 层负责 marshal/unmarshal。
type Task struct {
	ID        int64
	SessionID int
	Cmd       string
	Params    string // JSON 对象字符串, 可为 ""
	RequestID string
	Mode      string // push|pull
	Status    TaskStatus
	Result    string // 响应 JSON 或错误描述
	CreatedAt int64  // Unix 毫秒
	UpdatedAt int64
}

// taskStore 说明: 任务表字段直接内联在 Store(见 store.go), 本文件只提供
// 任务相关方法; SQLite 落盘在 store_persist.go。

// SaveTask 插入新任务(返回分配 ID)或更新已有任务。
func (s *Store) SaveTask(t *Task) (int64, error) {
	now := time.Now().UnixMilli()
	s.taskMu.Lock()
	if t.ID == 0 {
		s.nextTaskID++
		t.ID = s.nextTaskID
		t.CreatedAt = now
	}
	t.UpdatedAt = now
	s.tasks[t.ID] = t
	persist := s.persist
	s.taskMu.Unlock()

	if persist != nil {
		if err := persist.SaveTask(t); err != nil {
			return t.ID, err
		}
	}
	return t.ID, nil
}

// UpdateTaskStatus 更新任务状态与结果。
// **无条件写** —— 仅限无并发竞争的迁移(崩溃恢复加载期)或明确的覆盖意图;
// 等待者→终态的一切迁移必须走 UpdateTaskStatusIf(下), 否则并发下撕裂状态机
// (supersede 与响应回写互相覆盖一类的竞态, 第 3 批收口前真实存在)。
func (s *Store) UpdateTaskStatus(id int64, status TaskStatus, result string) {
	s.taskMu.Lock()
	t, ok := s.tasks[id]
	if !ok {
		s.taskMu.Unlock()
		return
	}
	t.Status = status
	t.Result = result
	t.UpdatedAt = time.Now().UnixMilli()
	persist := s.persist
	s.taskMu.Unlock()

	if persist != nil {
		_ = persist.UpdateTaskStatus(id, status, result, t.UpdatedAt)
	}
}

// UpdateTaskStatusIf 条件状态迁移(CAS, 第 3 批状态机收口 ): 仅当前状态
// 恰为 from 时更新为 to, 返回是否成功。
// **状态机铁律**: 每个迁移边只有一个合法发起者, 全部经本方法原子完成 ——
//
//	pending→dispatched(dispatchTask) / dispatched→completed|failed|pending|timeout
//	(等待 goroutine) / pending|dispatched→superseded(supersedeDuplicates) /
//	timeout→completed|failed(reviveLateResponse) / dispatched→timeout(SweepStuckTasks)。
//
// CAS 失败 = 该迁移已被别人完成(响应先到/已被取代/已被保险丝扫掉) —— 放弃即正确,
// **绝不**降级为无条件写。
func (s *Store) UpdateTaskStatusIf(id int64, from, to TaskStatus, result string) bool {
	s.taskMu.Lock()
	t, ok := s.tasks[id]
	if !ok || t.Status != from {
		s.taskMu.Unlock()
		return false
	}
	t.Status = to
	t.Result = result
	t.UpdatedAt = time.Now().UnixMilli()
	persist := s.persist
	idCopy, ts := t.ID, t.UpdatedAt
	s.taskMu.Unlock()

	if persist != nil {
		_ = persist.UpdateTaskStatus(idCopy, to, result, ts)
	}
	return true
}

// GetTask 按 ID 取任务副本。
func (s *Store) GetTask(id int64) (*Task, bool) {
	s.taskMu.Lock()
	defer s.taskMu.Unlock()
	t, ok := s.tasks[id]
	if !ok {
		return nil, false
	}
	cp := *t
	return &cp, true
}

// TasksBySession 返回某会话全部任务(按创建时间升序)。
func (s *Store) TasksBySession(sid int) []Task {
	s.taskMu.Lock()
	defer s.taskMu.Unlock()
	out := make([]Task, 0)
	for _, t := range s.tasks {
		if t.SessionID == sid {
			out = append(out, *t)
		}
	}
	sortTasks(out)
	return out
}

// PendingTasks 返回某会话待补派任务。
// compensate 循环里再过滤 dispatched —— "数据层宽松、逻辑层收紧"的隐性依赖: 新调用方
// 直接消费返回值就会对 dispatched 任务重派(双执行)。语义收紧为 pending-only,
// **不要**在调用方再兜一层过滤; 若未来需要"含在途"视图, 加独立方法而不是放宽本方法。
func (s *Store) PendingTasks(sid int) []Task {
	s.taskMu.Lock()
	defer s.taskMu.Unlock()
	out := make([]Task, 0)
	for _, t := range s.tasks {
		if t.SessionID == sid && t.Status == TaskPending {
			out = append(out, *t)
		}
	}
	sortTasks(out)
	return out
}

// TaskByRequestID 按 request_id 查任务(O(n) 遍历)。用于晚到响应复活
// (session_recv.go: 响应到达时 PendingResp 已无等待者, 靠本方法找到 timeout 任务复活)。
// 任务量级 ~百, 且仅"响应无人等"的丢弃路径触发, 常态热路径不走这里。
// 找不到或 rid 为空返回 (nil, false)。
func (s *Store) TaskByRequestID(rid string) (*Task, bool) {
	if rid == "" {
		return nil, false
	}
	s.taskMu.Lock()
	defer s.taskMu.Unlock()
	for _, t := range s.tasks {
		if t.RequestID == rid {
			cp := *t
			return &cp, true
		}
	}
	return nil, false
}

// AllTasks 返回全部任务(按创建时间升序)。
func (s *Store) AllTasks() []Task {
	s.taskMu.Lock()
	defer s.taskMu.Unlock()
	out := make([]Task, 0, len(s.tasks))
	for _, t := range s.tasks {
		out = append(out, *t)
	}
	sortTasks(out)
	return out
}

// sortTasks 按 ID 升序(等价创建顺序)。
func sortTasks(ts []Task) {
	for i := 1; i < len(ts); i++ {
		for j := i; j > 0 && ts[j].ID < ts[j-1].ID; j-- {
			ts[j], ts[j-1] = ts[j-1], ts[j]
		}
	}
}
