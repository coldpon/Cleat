// Package auth 操作员认证与角色授权(RBAC)。
// 认证: Authorization: Bearer <token> 解析为操作员身份; 兜底 token(-token)视为内置 admin。
// 授权: viewer 只读, operator/admin 可写; 高危命令(在 Manager DefineHighRisk)需 admin。
package auth

import (
	"context"
	"crypto/subtle"
	"net/http"
	"strings"

	"golang.org/x/crypto/bcrypt"

	"cleat/teamserver/internal/store"
)

type ctxKey int

const identityKey ctxKey = 0

// Identity 是一次请求解析出的操作员身份。
type Identity struct {
	Username      string
	Role          store.OperatorRole
	Authenticated bool
}

// FallbackAdminName 兜底 token(-token)映射为的内置管理员名。
const FallbackAdminName = "root"

// Authenticator 解析 Bearer token 得到操作员身份。
type Authenticator struct {
	st            *store.Store
	fallbackToken string
}

// New 创建认证器。fallbackToken 非空时视为内置 admin(兼容旧 -token 单一令牌)。
func New(st *store.Store, fallbackToken string) *Authenticator {
	return &Authenticator{st: st, fallbackToken: fallbackToken}
}

// HashPassword 生成 bcrypt 密码哈希(种子 admin / 操作员改密用)。
func HashPassword(plain string) (string, error) {
	b, err := bcrypt.GenerateFromPassword([]byte(plain), bcrypt.DefaultCost)
	return string(b), err
}

// ComparePassword 校验密码是否匹配哈希。
func ComparePassword(hash, plain string) error {
	return bcrypt.CompareHashAndPassword([]byte(hash), []byte(plain))
}

// Login 按用户名+密码认证, 返回操作员身份与其 API 令牌。
// 成功返回 (Identity, token, true); 帐号不存在/未激活/密码不符返回 ok=false。
func (a *Authenticator) Login(username, password string) (Identity, string, bool) {
	if a == nil || a.st == nil {
		return Identity{}, "", false
	}
	o, ok := a.st.Operator(username)
	if !ok || !o.Active {
		return Identity{}, "", false
	}
	if o.PasswordHash == "" {
		return Identity{}, "", false // 该帐号未设密码(仅 token 认证)
	}
	if bcrypt.CompareHashAndPassword([]byte(o.PasswordHash), []byte(password)) != nil {
		return Identity{}, "", false
	}
	if o.Token == "" {
		// 有密码但无 token: 不允许登录(需先由 admin 配置 token)
		return Identity{}, "", false
	}
	return Identity{Username: o.Username, Role: o.Role, Authenticated: true}, o.Token, true
}

// Identity 按 token 解析身份。token 不匹配任何操作员且不等于兜底 token 时返回 (!, ok=false)。
func (a *Authenticator) Identity(token string) (Identity, bool) {
	if a == nil {
		// 无认证配置: 保守只读, 防止误部署时在线会话被任意下发命令。
		return Identity{Username: "unknown", Role: store.RoleViewer, Authenticated: true}, true
	}
	if token == "" {
		return Identity{}, false
	}
	// 恒定时间比较: == 逐字节短路, 计时侧信道可逐字节推断令牌
	if a.fallbackToken != "" &&
		subtle.ConstantTimeCompare([]byte(token), []byte(a.fallbackToken)) == 1 {
		return Identity{Username: FallbackAdminName, Role: store.RoleAdmin, Authenticated: true}, true
	}
	if a.st != nil {
		if o, ok := a.st.OperatorByToken(token); ok {
			if !o.Active {
				return Identity{}, false // 已禁用的操作员令牌无效
			}
			return Identity{Username: o.Username, Role: o.Role, Authenticated: true}, true
		}
	}
	return Identity{}, false
}

// OperatorCount 返回当前已配置的操作员数量(用于判断是否处于开发无认证态)。
func (a *Authenticator) OperatorCount() int {
	if a == nil || a.st == nil {
		return 0
	}
	return len(a.st.Operators())
}

// OpSummary 操作员公开摘要(不含令牌), 供列表接口返回。
type OpSummary struct {
	Username string             `json:"username"`
	Role     store.OperatorRole `json:"role"`
	Active   bool               `json:"active"`
}

// ListOperators 返回操作员公开列表(脱敏: 不含 Token)。
func (a *Authenticator) ListOperators() []OpSummary {
	if a == nil || a.st == nil {
		return nil
	}
	ops := a.st.Operators()
	out := make([]OpSummary, 0, len(ops))
	for _, o := range ops {
		out = append(out, OpSummary{Username: o.Username, Role: o.Role, Active: o.Active})
	}
	return out
}

// ContextIdentity 从请求头 Authorization 或 ?token= query 解析身份并放入 context。
// 有效则返回 identity + ok=true; 无效返回 ok=false(调用方应 401)。
func (a *Authenticator) ContextIdentity(r *http.Request) (Identity, bool) {
	tok := identityToken(r)
	id, ok := a.Identity(tok)
	if !ok {
		return Identity{}, false
	}
	return id, true
}

// WithIdentity 把身份写入 context。
func WithIdentity(ctx context.Context, id Identity) context.Context {
	return context.WithValue(ctx, identityKey, id)
}

// FromContext 从 context 取身份。
func FromContext(ctx context.Context) (Identity, bool) {
	id, ok := ctx.Value(identityKey).(Identity)
	return id, ok && id.Authenticated
}

// CanWrite 校验身份可写角色。
func (id Identity) CanWrite() bool {
	return id.Role.CanWrite()
}

// IsAdmin 校验身份为管理员。
func (id Identity) IsAdmin() bool {
	return id.Role == store.RoleAdmin
}

// identityToken 提取凭据: 优先 Authorization: Bearer; ?token= query **仅对 SSE
// 事件流端点放行**: EventSource 无法带 header, 这是 query
// token 存在的唯一理由 —— 曾对所有端点放行, 全权令牌进 URL/访问日志/浏览器历史,
// 泄露即失陷。以 /events 后缀识别事件流端点(全局 /api/events 与会话级 .../events)。
func identityToken(r *http.Request) string {
	if t := bearerToken(r); t != "" {
		return t
	}
	if strings.HasSuffix(r.URL.Path, "/events") {
		if t := r.URL.Query().Get("token"); t != "" {
			return t
		}
	}
	return ""
}

// bearerToken 提取 Authorization: Bearer <token>。
func bearerToken(r *http.Request) string {
	h := r.Header.Get("Authorization")
	if !strings.HasPrefix(h, "Bearer ") {
		return ""
	}
	return strings.TrimSpace(h[len("Bearer "):])
}
