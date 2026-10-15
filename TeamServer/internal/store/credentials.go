// Package store 凭据仓库: Agent 采集 + 手动录入的凭据, 持久化到 SQLite。
// 参考 Havoc Token Vault + Adaptix Credentials Manager。
package store

import (
	"sort"
	"time"
)

// CredentialType 凭据类型。
type CredentialType string

const (
	CredTypePassword CredentialType = "password"
	CredTypeHash     CredentialType = "hash"
	CredTypeToken    CredentialType = "token"
	CredTypeTicket   CredentialType = "ticket"
	CredTypeKey      CredentialType = "key"
	CredTypeOther    CredentialType = "other"
)

// Credential 存储一条凭据(来自 CredentialData 推送或手动录入)。
type Credential struct {
	ID        int64          `json:"id"`
	Username  string         `json:"username"`
	Realm     string         `json:"realm"`      // 域/工作组
	CredType  CredentialType `json:"cred_type"`  // password/hash/token/ticket/key
	Secret    string         `json:"secret"`     // 密码/哈希/token 值
	Host      string         `json:"host"`       // 来源主机
	Source    string         `json:"source"`     // 来源方式: wifi/sam/browser/manual
	AgentID   int            `json:"agent_id"`   // 采集会话 ID(0=手动)
	Tag       string         `json:"tag"`        // 自定义标签
	CreatedAt int64          `json:"created_at"` // Unix 毫秒
}

// SaveCredential 存储一条凭据。
func (s *Store) SaveCredential(c *Credential) {
	s.credMu.Lock()
	if c.ID == 0 {
		c.ID = s.nextCredID
		s.nextCredID++
	}
	if c.CreatedAt == 0 {
		c.CreatedAt = time.Now().UnixMilli()
	}
	s.credentials = append(s.credentials, *c)
	cred := *c
	s.credMu.Unlock()
	if s.persist != nil {
		_ = s.persist.SaveCredential(&cred)
	}
}

// Credentials 返回所有凭据(按时间倒序)。
func (s *Store) Credentials() []Credential {
	s.credMu.Lock()
	defer s.credMu.Unlock()
	out := make([]Credential, len(s.credentials))
	copy(out, s.credentials)
	sort.Slice(out, func(i, j int) bool { return out[i].CreatedAt > out[j].CreatedAt })
	return out
}

// DeleteCredential 删除一条凭据。
func (s *Store) DeleteCredential(id int64) bool {
	s.credMu.Lock()
	defer s.credMu.Unlock()
	for i, c := range s.credentials {
		if c.ID == id {
			s.credentials = append(s.credentials[:i], s.credentials[i+1:]...)
			if s.persist != nil {
				_ = s.persist.DeleteCredential(id)
			}
			return true
		}
	}
	return false
}

// ClearCredentials 清空所有凭据。
func (s *Store) ClearCredentials() {
	s.credMu.Lock()
	s.credentials = nil
	s.credMu.Unlock()
	if s.persist != nil {
		_ = s.persist.ClearCredentials()
	}
}

// loadCredentials 启动时从 SQLite 加载凭据。
func (s *Store) loadCredentials() error {
	if s.persist == nil {
		return nil
	}
	creds, err := s.persist.LoadCredentials()
	if err != nil {
		return err
	}
	s.credMu.Lock()
	defer s.credMu.Unlock()
	s.credentials = creds
	if len(creds) > 0 {
		s.nextCredID = creds[len(creds)-1].ID + 1
	}
	return nil
}
