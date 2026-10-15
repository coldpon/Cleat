// 目标/资产管理: 记录渗透测试中发现的主机、服务、端口等目标信息。
// 参考 Havoc Targets Manager + Adaptix Targets Manager。
package store

import (
	"sort"
	"time"
)

// TargetStatus 目标状态。
type TargetStatus string

const (
	TargetStatusNew         TargetStatus = "new"
	TargetStatusScanned     TargetStatus = "scanned"
	TargetStatusCompromised TargetStatus = "compromised"
	TargetStatusClosed      TargetStatus = "closed"
)

// Target 存储一个目标/资产。
type Target struct {
	ID        int64        `json:"id"`
	IP        string       `json:"ip"`
	Hostname  string       `json:"hostname"`
	OS        string       `json:"os"`
	Ports     string       `json:"ports"`      // 逗号分隔: "80,443,3389"
	Protocol  string       `json:"protocol"`   // tcp/udp
	Status    TargetStatus `json:"status"`     // new/scanned/compromised/closed
	Tags      string       `json:"tags"`       // 逗号分隔标签
	Notes     string       `json:"notes"`      // 备注
	AgentID   int          `json:"agent_id"`   // 关联会话 ID(0=手动)
	CreatedAt int64        `json:"created_at"` // Unix 毫秒
	UpdatedAt int64        `json:"updated_at"`
}

// SaveTarget 存储一个目标。
func (s *Store) SaveTarget(t *Target) {
	s.targetMu.Lock()
	if t.ID == 0 {
		t.ID = s.nextTargetID
		s.nextTargetID++
	}
	now := time.Now().UnixMilli()
	if t.CreatedAt == 0 {
		t.CreatedAt = now
	}
	t.UpdatedAt = now
	s.targets = append(s.targets, *t)
	target := *t
	s.targetMu.Unlock()
	if s.persist != nil {
		_ = s.persist.SaveTarget(&target)
	}
}

// Targets 返回所有目标(按创建时间倒序)。
func (s *Store) Targets() []Target {
	s.targetMu.Lock()
	defer s.targetMu.Unlock()
	out := make([]Target, len(s.targets))
	copy(out, s.targets)
	sort.Slice(out, func(i, j int) bool { return out[i].CreatedAt > out[j].CreatedAt })
	return out
}

// UpdateTarget 更新目标。
func (s *Store) UpdateTarget(t *Target) bool {
	s.targetMu.Lock()
	defer s.targetMu.Unlock()
	for i, existing := range s.targets {
		if existing.ID == t.ID {
			t.UpdatedAt = time.Now().UnixMilli()
			s.targets[i] = *t
			if s.persist != nil {
				_ = s.persist.SaveTarget(t)
			}
			return true
		}
	}
	return false
}

// DeleteTarget 删除一个目标。
func (s *Store) DeleteTarget(id int64) bool {
	s.targetMu.Lock()
	defer s.targetMu.Unlock()
	for i, t := range s.targets {
		if t.ID == id {
			s.targets = append(s.targets[:i], s.targets[i+1:]...)
			if s.persist != nil {
				_ = s.persist.DeleteTarget(id)
			}
			return true
		}
	}
	return false
}

// ClearTargets 清空所有目标。
func (s *Store) ClearTargets() {
	s.targetMu.Lock()
	s.targets = nil
	s.targetMu.Unlock()
	if s.persist != nil {
		_ = s.persist.ClearTargets()
	}
}

// loadTargets 启动时从 SQLite 加载目标。
func (s *Store) loadTargets() error {
	if s.persist == nil {
		return nil
	}
	targets, err := s.persist.LoadTargets()
	if err != nil {
		return err
	}
	s.targetMu.Lock()
	defer s.targetMu.Unlock()
	s.targets = targets
	if len(targets) > 0 {
		s.nextTargetID = targets[len(targets)-1].ID + 1
	}
	return nil
}
