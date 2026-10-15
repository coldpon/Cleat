// 操作员身份模型 + 角色(RBAC)。
// 业界 C2 多操作员协作: 操作员有身份角色(admin/operator/viewer), 命令/审计带操作者。
// 认证令牌由部署注入(不硬编码), 与 PSK/token 的注入原则一致。
package store

import (
	"crypto/subtle"
	"errors"
	"sort"
	"time"
)

// errInvalidOperator 操作员字段不合法。
var errInvalidOperator = errors.New("操作员需有用户名/合法角色/令牌")

// OperatorRole 操作员角色。
type OperatorRole string

const (
	// RoleAdmin 管理员: 全量权限, 可管理操作员。
	RoleAdmin OperatorRole = "admin"
	// RoleOperator 操作员: 可查看会话并下发/执行命令。
	RoleOperator OperatorRole = "operator"
	// RoleViewer 观察员: 只读, 不能下发命令。
	RoleViewer OperatorRole = "viewer"
)

// Operator 是一名操作员身份。
type Operator struct {
	Username     string
	Role         OperatorRole
	Token        string // API 访问令牌(由部署通过 flag/配置注入或登录返回)
	PasswordHash string // bcrypt 哈希(登录用; 同时可配 Token 走 bearer)
	Active       bool
	CreatedAt    int64 // Unix 毫秒
}

// Valid 校验操作员字段: 需有用户名 + 合法角色 + (令牌或密码哈希至少其一)。
func (o *Operator) Valid() bool {
	roleOK := o.Role == RoleAdmin || o.Role == RoleOperator || o.Role == RoleViewer
	return o.Username != "" && roleOK && (o.Token != "" || o.PasswordHash != "")
}

// CanWrite 该角色是否可下发命令(admin/operator 可写, viewer 只读)。
func (r OperatorRole) CanWrite() bool {
	return r == RoleAdmin || r == RoleOperator
}

// SaveOperator 新增或更新一个操作员。
func (s *Store) SaveOperator(o *Operator) error {
	if !o.Valid() {
		return errInvalidOperator
	}
	s.opMu.Lock()
	if _, exists := s.operators[o.Username]; !exists {
		o.CreatedAt = time.Now().UnixMilli()
	}
	s.operators[o.Username] = o
	persist := s.persist
	s.opMu.Unlock()

	if persist != nil {
		return persist.SaveOperator(o)
	}
	return nil
}

// Operator 按用户名取操作员。
func (s *Store) Operator(username string) (*Operator, bool) {
	s.opMu.Lock()
	defer s.opMu.Unlock()
	o, ok := s.operators[username]
	if !ok {
		return nil, false
	}
	cp := *o
	return &cp, true
}

// OperatorByToken 按令牌取操作员(用于 API 认证)。空令牌不匹配。
func (s *Store) OperatorByToken(token string) (*Operator, bool) {
	if token == "" {
		return nil, false
	}
	s.opMu.Lock()
	defer s.opMu.Unlock()
	for _, o := range s.operators {
		// 恒定时间比较: == 短路比较存在计时侧信道
		if subtle.ConstantTimeCompare([]byte(o.Token), []byte(token)) == 1 {
			cp := *o
			return &cp, true
		}
	}
	return nil, false
}

// Operators 返回全部操作员(按用户名排序)。
func (s *Store) Operators() []Operator {
	s.opMu.Lock()
	defer s.opMu.Unlock()
	out := make([]Operator, 0, len(s.operators))
	for _, o := range s.operators {
		out = append(out, *o)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Username < out[j].Username })
	return out
}

// RemoveOperator 删除一个操作员; admin 用户不允许删除。
func (s *Store) RemoveOperator(username string) {
	s.opMu.Lock()
	old := s.operators[username]
	if old != nil && old.Role == RoleAdmin {
		s.opMu.Unlock()
		return
	}
	delete(s.operators, username)
	persist := s.persist
	s.opMu.Unlock()
	if persist != nil {
		_ = persist.RemoveOperator(username)
	}
}
