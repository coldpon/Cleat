// 目标管理 / 凭据管理 / 审计与事件历史查询用例。
// 搬运自 api/targets.go、api/credentials.go、api/audit.go。
package service

import (
	"cleat/teamserver/internal/store"
)

// TargetInput 目标录入参数(替代原 handler 内的匿名 req 结构, 避免 api 触碰 store 类型)。
type TargetInput struct {
	IP       string
	Hostname string
	OS       string
	Ports    string
	Protocol string
	Status   string // 仅 TargetUpdate 消费
	Tags     string
	Notes    string
	AgentID  int
}

// Targets 目标列表。搬运自 api.listTargets。
func (s *Service) Targets() []store.Target { return s.st.Targets() }

// TargetAdd 新增目标, 返回自增 ID。协议缺省补 tcp。搬运自 api.addTarget 主体。
func (s *Service) TargetAdd(op string, in TargetInput) int64 {
	if in.Protocol == "" {
		in.Protocol = "tcp"
	}
	t := &store.Target{
		IP:       in.IP,
		Hostname: in.Hostname,
		OS:       in.OS,
		Ports:    in.Ports,
		Protocol: in.Protocol,
		Tags:     in.Tags,
		Notes:    in.Notes,
		AgentID:  in.AgentID,
	}
	s.st.SaveTarget(t)
	return t.ID
}

// TargetUpdate 更新目标(非空字段覆盖语义, 与原实现逐字段一致)。返回 false=不存在(404)。
// 搬运自 api.updateTarget 主体(含全表扫描定位的原始实现, 优化属后续批次)。
func (s *Service) TargetUpdate(op string, id int64, in TargetInput) bool {
	// 查找现有目标
	existing := s.st.Targets()
	var t *store.Target
	for _, et := range existing {
		if et.ID == id {
			et := et
			t = &et
			break
		}
	}
	if t == nil {
		return false
	}
	if in.IP != "" {
		t.IP = in.IP
	}
	if in.Hostname != "" {
		t.Hostname = in.Hostname
	}
	if in.OS != "" {
		t.OS = in.OS
	}
	if in.Ports != "" {
		t.Ports = in.Ports
	}
	if in.Protocol != "" {
		t.Protocol = in.Protocol
	}
	if in.Status != "" {
		t.Status = store.TargetStatus(in.Status)
	}
	if in.Tags != "" {
		t.Tags = in.Tags
	}
	if in.Notes != "" {
		t.Notes = in.Notes
	}
	return s.st.UpdateTarget(t)
}

// TargetDelete 删除目标。false=不存在(404)。
func (s *Service) TargetDelete(op string, id int64) bool { return s.st.DeleteTarget(id) }

// TargetClear 清空目标(入口层已限 admin)。
func (s *Service) TargetClear(op string) { s.st.ClearTargets() }

// CredentialInput 凭据录入参数(同 TargetInput 理由)。
type CredentialInput struct {
	Username string
	Realm    string
	CredType string
	Secret   string
	Host     string
	Source   string
	AgentID  int
	Tag      string
}

// Credentials 凭据列表(入口层已限 admin)。
func (s *Service) Credentials() []store.Credential { return s.st.Credentials() }

// CredentialAdd 新增凭据, 类型缺省补 password。返回自增 ID。搬运自 api.addCredential 主体。
func (s *Service) CredentialAdd(op string, in CredentialInput) int64 {
	if in.CredType == "" {
		in.CredType = "password"
	}
	c := &store.Credential{
		Username: in.Username,
		Realm:    in.Realm,
		CredType: store.CredentialType(in.CredType),
		Secret:   in.Secret,
		Host:     in.Host,
		Source:   in.Source,
		AgentID:  in.AgentID,
		Tag:      in.Tag,
	}
	s.st.SaveCredential(c)
	return c.ID
}

// CredentialDelete 删除凭据。false=不存在(404)。
func (s *Service) CredentialDelete(op string, id int64) bool { return s.st.DeleteCredential(id) }

// CredentialClear 清空凭据(入口层已限 admin)。
func (s *Service) CredentialClear(op string) { s.st.ClearCredentials() }

// AuditList 审计日志查询(分页+过滤)。搬运自 api.auditList 数据源。
func (s *Service) AuditList(f AuditFilter) ([]store.AuditRecord, int, error) {
	return s.st.ListAudit(f)
}

// EventsHistory 历史事件查询。搬运自 api.eventsHistory 数据源。
func (s *Service) EventsHistory(f EventFilter) ([]store.EventRecord, int, error) {
	return s.st.QueryEvents(f)
}
