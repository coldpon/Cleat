package store

import (
	"context"
	"database/sql"
	"fmt"
	"os"
	"path/filepath"
	"time"

	"cleat/teamserver/internal/logx"
	_ "modernc.org/sqlite" // 纯 Go SQLite(免 CGO)
)

// Persistence 是会话持久化后端接口(落盘用途)。
type Persistence interface {
	// LoadSessions 启动时加载历史会话(恢复 machine_id->ID 映射 + 主机信息)。
	LoadSessions() ([]*Record, error)
	// SaveSession 持久化/更新一个会话。
	SaveSession(r *Record) error
	// DeleteSession 从持久化层删除一个会话(cleanup 清理离线记录时同步落盘,
	// 否则历史记录每次启动都会重新加载并被重复"删除")。
	DeleteSession(id int) error
	// LoadTasks 启动时加载历史任务(恢复未完成任务供上线补偿)。
	LoadTasks() ([]Task, error)
	// SaveTask 持久化/更新一个任务。
	SaveTask(t *Task) error
	// UpdateTaskStatus 更新任务状态与结果。
	UpdateTaskStatus(id int64, status TaskStatus, result string, updatedAt int64) error
	// LoadOperators 启动时加载全部操作员。
	LoadOperators() ([]Operator, error)
	// SaveOperator 持久化一个操作员。
	SaveOperator(o *Operator) error
	// RemoveOperator 删除一个操作员。
	RemoveOperator(username string) error
	// 凭据
	LoadCredentials() ([]Credential, error)
	SaveCredential(c *Credential) error
	DeleteCredential(id int64) error
	ClearCredentials() error
	// 目标
	LoadTargets() ([]Target, error)
	SaveTarget(t *Target) error
	DeleteTarget(id int64) error
	ClearTargets() error
}

// sqlitePersistence 基于 SQLite 的实现。
type sqlitePersistence struct {
	db *sql.DB
}

// WithSQLite 打开指定路径的 SQLite 数据库并作为持久化后端。
// schema 由 goose 嵌入式迁移管理(migrations.go + migrations/) —— 版本记账在
// goose_db_version 表, 启动即知新旧。**防重蹈覆辙**: 不要再往本函数加 CREATE/ALTER ——
// 曾靠 `_, _ = db.Exec(ALTER...)` 吞错误当版本管理, 真失败也被吞; 一切变更走新迁移文件。
// 返回 *Store 并预加载历史会话。
func WithSQLite(path string) (*Store, error) {
	// 自愈: 父目录不存在(用户手滑删了 data/ 等)时 SQLite 会报
	// "unable to open database file" 直接起不来 —— 先建目录再打开。
	if dir := filepath.Dir(path); dir != "" && dir != "." {
		if err := os.MkdirAll(dir, 0o755); err != nil {
			return nil, fmt.Errorf("创建数据库目录失败: %w", err)
		}
	}
	db, err := sql.Open("sqlite", path)
	if err != nil {
		return nil, fmt.Errorf("打开 SQLite 失败: %w", err)
	}
	if err := db.Ping(); err != nil {
		return nil, fmt.Errorf("SQLite Ping 失败: %w", err)
	}
	// WAL 模式: 读写并发友好, 崩溃安全
	_, _ = db.Exec("PRAGMA journal_mode=WAL;")
	ver, err := gooseUp(context.Background(), db)
	if err != nil {
		return nil, fmt.Errorf("schema 迁移失败: %w", err)
	}
	logx.Printf("[*] 数据表就绪（schema v%d）\n", ver)
	p := &sqlitePersistence{db: db}

	st := New()
	st.persist = p
	if err := st.loadFromPersistence(); err != nil {
		return nil, err
	}
	if err := st.loadTasksFromPersistence(); err != nil {
		return nil, err
	}
	if err := st.loadOperatorsFromPersistence(); err != nil {
		return nil, err
	}
	if err := st.loadCredentials(); err != nil {
		return nil, err
	}
	if err := st.loadTargets(); err != nil {
		return nil, err
	}
	return st, nil
}

// loadOperatorsFromPersistence 启动时加载全部操作员到内存。
func (s *Store) loadOperatorsFromPersistence() error {
	if s.persist == nil {
		return nil
	}
	ops, err := s.persist.LoadOperators()
	if err != nil {
		return err
	}
	s.opMu.Lock()
	defer s.opMu.Unlock()
	for _, o := range ops {
		cp := o
		s.operators[cp.Username] = &cp
	}
	return nil
}

// loadTasksFromPersistence 启动时加载历史任务, 未完成任务(pending/dispatched)重置为 pending
// 供 Agent 上线时补偿(整表加载到内存后由 session 层按会话补偿)。
func (s *Store) loadTasksFromPersistence() error {
	if s.persist == nil {
		return nil
	}
	tasks, err := s.persist.LoadTasks()
	if err != nil {
		return err
	}
	s.taskMu.Lock()
	defer s.taskMu.Unlock()
	for _, t := range tasks {
		cp := t
		if cp.Status == TaskDispatched {
			cp.Status = TaskPending // 崩溃重启: 在途任务无法确证, 重置为待派发
		}
		s.tasks[cp.ID] = &cp
		if cp.ID >= s.nextTaskID {
			s.nextTaskID = cp.ID + 1
		}
	}
	return nil
}

// loadFromPersistence 启动时加载历史会话到内存(供重连复用)。
func (s *Store) loadFromPersistence() error {
	if s.persist == nil {
		return nil
	}
	recs, err := s.persist.LoadSessions()
	if err != nil {
		return err
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, r := range recs {
		s.sessions[r.ID] = r
		if r.MachineID != "" {
			s.machine[r.MachineID] = r.ID
		}
		if r.ID >= s.nextID {
			s.nextID = r.ID + 1 // 避免 ID 冲突
		}
	}
	return nil
}

// Checkpoint 立即把当前所有会话持久化(可周期性/退出时调用)。
func (s *Store) Checkpoint() {
	// A3-20: 曾持 RLock 期间逐条 SaveSession 做 SQLite I/O ——
	// 期间所有写路径(Put/删除)全被阻塞。锁内只拷贝指针快照, I/O 移到锁外。
	s.mu.RLock()
	if s.persist == nil {
		s.mu.RUnlock()
		return
	}
	snapshot := make([]*Record, 0, len(s.sessions))
	for _, r := range s.sessions {
		snapshot = append(snapshot, r)
	}
	s.mu.RUnlock()
	for _, r := range snapshot {
		_ = s.persist.SaveSession(r)
	}
}

// Close 关闭持久化后端。
func (s *Store) Close() error {
	if s.persist == nil {
		return nil
	}
	if p, ok := s.persist.(*sqlitePersistence); ok {
		return p.db.Close()
	}
	return nil
}

// Audit 记录操作审计(仅持久化后端支持; 纯内存忽略)。
func (s *Store) Audit(operator string, sessionID int, cmd, mode, status string) {
	if p, ok := s.persist.(*sqlitePersistence); ok {
		p.Audit(time.Now().UnixMilli(), operator, sessionID, cmd, mode, status)
	}
}

// AuditRecord 一条审计日志(查询返回)。
type AuditRecord struct {
	ID        int64  `json:"id"`
	TS        int64  `json:"ts"`
	Operator  string `json:"operator"`
	SessionID int    `json:"session_id"`
	Cmd       string `json:"cmd"`
	Mode      string `json:"mode"`
	Status    string `json:"status"`
}

// AuditFilter 审计查询过滤条件。
type AuditFilter struct {
	Operator  string
	SessionID *int
	Cmd       string
	Mode      string
	Status    string
	Start     int64 // 0 = 不限
	End       int64 // 0 = 不限
	Limit     int
	Offset    int
}

// ListAudit 分页查询审计日志(仅持久化后端; 纯内存返回空)。
// 返回 (记录, 总数)。
func (s *Store) ListAudit(f AuditFilter) ([]AuditRecord, int, error) {
	if p, ok := s.persist.(*sqlitePersistence); ok {
		return p.ListAudit(f)
	}
	return nil, 0, nil
}

// EventRecord 落盘事件(结果型事件)。
type EventRecord struct {
	ID        int64  `json:"id"`
	Seq       uint64 `json:"seq"`
	TS        int64  `json:"ts"`
	Type      string `json:"type"`
	SessionID int    `json:"session_id"`
	Data      string `json:"data"` // 事件负载 JSON 文本; 展示失败时回退原文
}

// EventFilter 历史事件查询过滤条件。
type EventFilter struct {
	Type      string
	SessionID *int
	Start     int64
	End       int64
	Limit     int
	Offset    int
}

// AppendEvent 落盘一条事件(仅持久化后端; 纯内存忽略)。
func (s *Store) AppendEvent(ev EventRecord) {
	if p, ok := s.persist.(*sqlitePersistence); ok {
		p.InsertEvent(ev)
	}
}

// QueryEvents 分页查询历史事件(仅持久化后端; 纯内存返回空)。
// 返回 (记录, 总数)。
func (s *Store) QueryEvents(f EventFilter) ([]EventRecord, int, error) {
	if p, ok := s.persist.(*sqlitePersistence); ok {
		return p.QueryEvents(f)
	}
	return nil, 0, nil
}

// LoadSessions 从 SQLite 读取全部会话。
func (p *sqlitePersistence) LoadSessions() ([]*Record, error) {
	rows, err := p.db.Query(`SELECT id, machine_id, hostname, os, os_version, cpu,
		memory, local_ip, arch, pid, COALESCE(process_name, ''), COALESCE(privilege, ''), last_seen,
		COALESCE(transport, '') FROM sessions`)
	if err != nil {
		return nil, err
	}
	defer func() { _ = rows.Close() }()
	var out []*Record
	for rows.Next() {
		var r Record
		var lastSeen int64
		var machineID *string
		if err := rows.Scan(&r.ID, &machineID, &r.Hostname, &r.OS, &r.OSVersion,
			&r.CPU, &r.Memory, &r.LocalIP, &r.Arch, &r.PID, &r.ProcessName, &r.Privilege, &lastSeen,
			&r.Transport); err != nil {
			return nil, err
		}
		if machineID != nil {
			r.MachineID = *machineID
		}
		if lastSeen > 0 {
			r.LastSeen = time.UnixMilli(lastSeen)
		}
		r.Closed = true // 历史会话初始视为离线, 等待 Agent 重连激活
		out = append(out, &r)
	}
	return out, rows.Err()
}

// SaveSession 写入/更新一个会话。
func (p *sqlitePersistence) SaveSession(r *Record) error {
	// LastSnapshot 在 infoMu 锁内取: LastSeen 被 Touch() 并发热写, 裸读是数据竞争
	// (其余字段注册后不再变, 无需快照)。
	r.Lock()
	lastSeen := r.LastSeen
	r.Unlock()
	_, err := p.db.Exec(`INSERT INTO sessions (
		id, machine_id, hostname, os, os_version, cpu, memory, local_ip, arch,
		pid, process_name, privilege, last_seen, transport)
		VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
		ON CONFLICT(id) DO UPDATE SET
		  machine_id=excluded.machine_id, hostname=excluded.hostname, os=excluded.os,
		  os_version=excluded.os_version, cpu=excluded.cpu, memory=excluded.memory,
		  local_ip=excluded.local_ip, arch=excluded.arch,
		  pid=excluded.pid, process_name=excluded.process_name, privilege=excluded.privilege,
		  last_seen=excluded.last_seen, transport=excluded.transport`,
		r.ID, nail(r.MachineID), r.Hostname, r.OS, r.OSVersion, r.CPU,
		r.Memory, r.LocalIP, r.Arch, r.PID, nail(r.ProcessName), nail(r.Privilege), lastSeen.UnixMilli(),
		nail(r.Transport))
	return err
}

// nail 空串转 nil(SQLite 存 NULL)。
func nail(s string) interface{} {
	if s == "" {
		return nil
	}
	return s
}

// DeleteSession 从 SQLite 删除一个会话记录。
func (p *sqlitePersistence) DeleteSession(id int) error {
	_, err := p.db.Exec(`DELETE FROM sessions WHERE id = ?`, id)
	return err
}

// LoadTasks 从 SQLite 读取全部任务。
func (p *sqlitePersistence) LoadTasks() ([]Task, error) {
	rows, err := p.db.Query(`SELECT id, session_id, cmd, params, request_id, mode,
		status, result, created_at, updated_at FROM tasks`)
	if err != nil {
		return nil, err
	}
	defer func() { _ = rows.Close() }()
	var out []Task
	for rows.Next() {
		var t Task
		var params, result *string
		if err := rows.Scan(&t.ID, &t.SessionID, &t.Cmd, &params, &t.RequestID,
			&t.Mode, &t.Status, &result, &t.CreatedAt, &t.UpdatedAt); err != nil {
			return nil, err
		}
		if params != nil {
			t.Params = *params
		}
		if result != nil {
			t.Result = *result
		}
		out = append(out, t)
	}
	return out, rows.Err()
}

// SaveTask 写入/更新一个任务。
func (p *sqlitePersistence) SaveTask(t *Task) error {
	_, err := p.db.Exec(`INSERT INTO tasks (id, session_id, cmd, params, request_id,
		mode, status, result, created_at, updated_at)
		VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
		ON CONFLICT(id) DO UPDATE SET
		  session_id=excluded.session_id, cmd=excluded.cmd, params=excluded.params,
		  request_id=excluded.request_id, mode=excluded.mode, status=excluded.status,
		  result=excluded.result, created_at=excluded.created_at, updated_at=excluded.updated_at`,
		t.ID, t.SessionID, t.Cmd, nail(t.Params), t.RequestID, t.Mode, t.Status,
		nail(t.Result), t.CreatedAt, t.UpdatedAt)
	return err
}

// UpdateTaskStatus 更新任务状态与结果。
func (p *sqlitePersistence) UpdateTaskStatus(id int64, status TaskStatus, result string, updatedAt int64) error {
	_, err := p.db.Exec(`UPDATE tasks SET status=?, result=?, updated_at=? WHERE id=?`,
		status, nail(result), updatedAt, id)
	return err
}

// LoadOperators 从 SQLite 读取全部操作员。
func (p *sqlitePersistence) LoadOperators() ([]Operator, error) {
	rows, err := p.db.Query(`SELECT username, role, token, password_hash, active, created_at FROM operators`)
	if err != nil {
		return nil, err
	}
	defer func() { _ = rows.Close() }()
	var out []Operator
	for rows.Next() {
		var o Operator
		var token, hash *string
		var active int
		if err := rows.Scan(&o.Username, &o.Role, &token, &hash, &active, &o.CreatedAt); err != nil {
			return nil, err
		}
		if token != nil {
			o.Token = *token
		}
		if hash != nil {
			o.PasswordHash = *hash
		}
		o.Active = active != 0
		out = append(out, o)
	}
	return out, rows.Err()
}

// SaveOperator 写入/更新一个操作员。
func (p *sqlitePersistence) SaveOperator(o *Operator) error {
	_, err := p.db.Exec(`INSERT INTO operators (username, role, token, password_hash, active, created_at)
		VALUES (?, ?, ?, ?, ?, ?)
		ON CONFLICT(username) DO UPDATE SET
		  role=excluded.role, token=excluded.token, password_hash=excluded.password_hash,
		  active=excluded.active, created_at=excluded.created_at`,
		o.Username, o.Role, nail(o.Token), nail(o.PasswordHash), boolInt(o.Active), o.CreatedAt)
	return err
}

// RemoveOperator 删除一个操作员。
func (p *sqlitePersistence) RemoveOperator(username string) error {
	_, err := p.db.Exec(`DELETE FROM operators WHERE username=?`, username)
	return err
}

// boolInt 布尔转 0/1(SQLite 存整数)。
func boolInt(b bool) int {
	if b {
		return 1
	}
	return 0
}

// Audit 记录一条操作审计(落盘后端存在时写 audit_log; 纯内存则忽略)。
// operator/mode/status 留由调用方填。
func (p *sqlitePersistence) Audit(ts int64, operator string, sessionID int, cmd, mode, status string) {
	_, _ = p.db.Exec(`INSERT INTO audit_log (ts, operator, session_id, cmd, mode, status)
		VALUES (?, ?, ?, ?, ?, ?)`,
		ts, nail(operator), sessionID, cmd, mode, status)
}

// ListAudit 分页查询审计日志。过滤条件按 AuditFilter 字段拼接, 返回 (记录, 总数)。
func (p *sqlitePersistence) ListAudit(f AuditFilter) ([]AuditRecord, int, error) {
	where := "WHERE 1=1"
	args := []interface{}{}
	if f.Operator != "" {
		where += " AND operator LIKE ?"
		args = append(args, "%"+f.Operator+"%")
	}
	if f.SessionID != nil {
		where += " AND session_id = ?"
		args = append(args, *f.SessionID)
	}
	if f.Cmd != "" {
		where += " AND cmd LIKE ?"
		args = append(args, "%"+f.Cmd+"%")
	}
	if f.Mode != "" {
		where += " AND mode = ?"
		args = append(args, f.Mode)
	}
	if f.Status != "" {
		where += " AND status = ?"
		args = append(args, f.Status)
	}
	if f.Start > 0 {
		where += " AND ts >= ?"
		args = append(args, f.Start)
	}
	if f.End > 0 {
		where += " AND ts <= ?"
		args = append(args, f.End)
	}
	limit, offset := f.Limit, f.Offset
	if limit <= 0 || limit > 500 {
		limit = 100
	}
	if offset < 0 {
		offset = 0
	}
	var total int
	_ = p.db.QueryRow(`SELECT COUNT(*) FROM audit_log `+where, args...).Scan(&total)
	rows, err := p.db.Query(`SELECT id, ts, operator, session_id, cmd, mode, status FROM audit_log `+
		where+` ORDER BY id DESC LIMIT ? OFFSET ?`, append(args, limit, offset)...)
	if err != nil {
		return nil, 0, err
	}
	defer func() { _ = rows.Close() }()
	var out []AuditRecord
	for rows.Next() {
		var a AuditRecord
		if err := rows.Scan(&a.ID, &a.TS, &a.Operator, &a.SessionID, &a.Cmd, &a.Mode, &a.Status); err != nil {
			return nil, 0, err
		}
		out = append(out, a)
	}
	return out, total, rows.Err()
}

// InsertEvent 落盘一条事件。
func (p *sqlitePersistence) InsertEvent(ev EventRecord) {
	_, _ = p.db.Exec(`INSERT INTO events (ts, seq, type, session_id, data) VALUES (?, ?, ?, ?, ?)`,
		ev.TS, ev.Seq, ev.Type, ev.SessionID, ev.Data)
}

// QueryEvents 分页查询历史事件, 按时间倒序。返回 (记录, 总数)。
func (p *sqlitePersistence) QueryEvents(f EventFilter) ([]EventRecord, int, error) {
	where := "WHERE 1=1"
	args := []interface{}{}
	if f.Type != "" {
		where += " AND type = ?"
		args = append(args, f.Type)
	}
	if f.SessionID != nil {
		where += " AND session_id = ?"
		args = append(args, *f.SessionID)
	}
	if f.Start > 0 {
		where += " AND ts >= ?"
		args = append(args, f.Start)
	}
	if f.End > 0 {
		where += " AND ts <= ?"
		args = append(args, f.End)
	}
	limit, offset := f.Limit, f.Offset
	if limit <= 0 || limit > 500 {
		limit = 200
	}
	if offset < 0 {
		offset = 0
	}
	var total int
	_ = p.db.QueryRow(`SELECT COUNT(*) FROM events `+where, args...).Scan(&total)
	rows, err := p.db.Query(`SELECT id, seq, ts, type, session_id, data FROM events `+
		where+` ORDER BY id DESC LIMIT ? OFFSET ?`, append(args, limit, offset)...)
	if err != nil {
		return nil, 0, err
	}
	defer func() { _ = rows.Close() }()
	var out []EventRecord
	for rows.Next() {
		var e EventRecord
		if err := rows.Scan(&e.ID, &e.Seq, &e.TS, &e.Type, &e.SessionID, &e.Data); err != nil {
			return nil, 0, err
		}
		out = append(out, e)
	}
	return out, total, rows.Err()
}

// ============ 凭据 CRUD ============

func (p *sqlitePersistence) LoadCredentials() ([]Credential, error) {
	rows, err := p.db.Query(`SELECT id, username, realm, cred_type, secret, host, source, agent_id, tag, created_at FROM credentials ORDER BY id`)
	if err != nil {
		return nil, err
	}
	defer func() { _ = rows.Close() }()
	var out []Credential
	for rows.Next() {
		var c Credential
		var username, realm, credType, secret, host, source, tag *string
		if err := rows.Scan(&c.ID, &username, &realm, &credType, &secret, &host, &source, &c.AgentID, &tag, &c.CreatedAt); err != nil {
			return nil, err
		}
		if username != nil {
			c.Username = *username
		}
		if realm != nil {
			c.Realm = *realm
		}
		if credType != nil {
			c.CredType = CredentialType(*credType)
		}
		if secret != nil {
			c.Secret = *secret
		}
		if host != nil {
			c.Host = *host
		}
		if source != nil {
			c.Source = *source
		}
		if tag != nil {
			c.Tag = *tag
		}
		out = append(out, c)
	}
	return out, rows.Err()
}

func (p *sqlitePersistence) SaveCredential(c *Credential) error {
	_, err := p.db.Exec(`INSERT INTO credentials (id, username, realm, cred_type, secret, host, source, agent_id, tag, created_at)
		VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
		ON CONFLICT(id) DO UPDATE SET
		  username=excluded.username, realm=excluded.realm, cred_type=excluded.cred_type,
		  secret=excluded.secret, host=excluded.host, source=excluded.source,
		  agent_id=excluded.agent_id, tag=excluded.tag, created_at=excluded.created_at`,
		c.ID, c.Username, c.Realm, string(c.CredType), c.Secret, c.Host, c.Source, c.AgentID, c.Tag, c.CreatedAt)
	return err
}

func (p *sqlitePersistence) DeleteCredential(id int64) error {
	_, err := p.db.Exec(`DELETE FROM credentials WHERE id=?`, id)
	return err
}

func (p *sqlitePersistence) ClearCredentials() error {
	_, err := p.db.Exec(`DELETE FROM credentials`)
	return err
}

// ============ 目标 CRUD ============

func (p *sqlitePersistence) LoadTargets() ([]Target, error) {
	rows, err := p.db.Query(`SELECT id, ip, hostname, os, ports, protocol, status, tags, notes, agent_id, created_at, updated_at FROM targets ORDER BY id`)
	if err != nil {
		return nil, err
	}
	defer func() { _ = rows.Close() }()
	var out []Target
	for rows.Next() {
		var t Target
		var ip, hostname, os, ports, protocol, status, tags, notes *string
		if err := rows.Scan(&t.ID, &ip, &hostname, &os, &ports, &protocol, &status, &tags, &notes, &t.AgentID, &t.CreatedAt, &t.UpdatedAt); err != nil {
			return nil, err
		}
		if ip != nil {
			t.IP = *ip
		}
		if hostname != nil {
			t.Hostname = *hostname
		}
		if os != nil {
			t.OS = *os
		}
		if ports != nil {
			t.Ports = *ports
		}
		if protocol != nil {
			t.Protocol = *protocol
		}
		if status != nil {
			t.Status = TargetStatus(*status)
		}
		if tags != nil {
			t.Tags = *tags
		}
		if notes != nil {
			t.Notes = *notes
		}
		out = append(out, t)
	}
	return out, rows.Err()
}

func (p *sqlitePersistence) SaveTarget(t *Target) error {
	_, err := p.db.Exec(`INSERT INTO targets (id, ip, hostname, os, ports, protocol, status, tags, notes, agent_id, created_at, updated_at)
		VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
		ON CONFLICT(id) DO UPDATE SET
		  ip=excluded.ip, hostname=excluded.hostname, os=excluded.os, ports=excluded.ports,
		  protocol=excluded.protocol, status=excluded.status, tags=excluded.tags, notes=excluded.notes,
		  agent_id=excluded.agent_id, created_at=excluded.created_at, updated_at=excluded.updated_at`,
		t.ID, t.IP, t.Hostname, t.OS, t.Ports, t.Protocol, string(t.Status), t.Tags, t.Notes, t.AgentID, t.CreatedAt, t.UpdatedAt)
	return err
}

func (p *sqlitePersistence) DeleteTarget(id int64) error {
	_, err := p.db.Exec(`DELETE FROM targets WHERE id=?`, id)
	return err
}

func (p *sqlitePersistence) ClearTargets() error {
	_, err := p.db.Exec(`DELETE FROM targets`)
	return err
}
