package auth

import (
	"net/http/httptest"
	"testing"

	"cleat/teamserver/internal/store"
)

func op(name string, role store.OperatorRole, tok string) *store.Operator {
	return &store.Operator{Username: name, Role: role, Token: tok, Active: true}
}

// TestIdentityResolution 验证操作员 token / 兜底 admin token / 未知 token 的解析与角色。
func TestIdentityResolution(t *testing.T) {
	st := store.New()
	if err := st.SaveOperator(op("alice", store.RoleOperator, "tok-alice")); err != nil {
		t.Fatal(err)
	}
	if err := st.SaveOperator(op("view", store.RoleViewer, "tok-view")); err != nil {
		t.Fatal(err)
	}
	if err := st.SaveOperator(op("gone", store.RoleOperator, "tok-gone")); err != nil {
		t.Fatal(err)
	}
	// 禁用 gone: 其令牌认证应失败
	if g, _ := st.Operator("gone"); g != nil {
		g.Active = false
		_ = st.SaveOperator(g)
	}

	a := New(st, "fallback-admin")
	if id, ok := a.Identity("tok-alice"); !ok || id.Username != "alice" {
		t.Fatalf("operator token 应解析为 alice, got %+v ok=%v", id, ok)
	}
	id, _ := a.Identity("tok-view")
	if id.CanWrite() {
		t.Fatal("viewer 不应可写")
	}
	if id, ok := a.Identity("fallback-admin"); !ok || !id.IsAdmin() {
		t.Fatalf("兜底 token 应解析为内置 admin")
	}
	if _, ok := a.Identity("bogus"); ok {
		t.Fatal("未知 token 不应通过")
	}
	if _, ok := a.Identity("tok-gone"); ok {
		t.Fatal("已禁用操作员的 token 不应通过")
	}
	if _, ok := a.Identity(""); ok {
		t.Fatal("空 token 不应通过")
	}
}

// TestLogin 验证密码登录: 正确/错误/未激活/无密码。
func TestLogin(t *testing.T) {
	st := store.New()
	hash, err := HashPassword("s3cret")
	if err != nil {
		t.Fatalf("HashPassword: %v", err)
	}
	if err := st.SaveOperator(&store.Operator{Username: "op", Role: store.RoleOperator, Token: "tok-op", PasswordHash: hash, Active: true}); err != nil {
		t.Fatal(err)
	}
	_ = st.SaveOperator(op("nogood", store.RoleViewer, "tok-no"))     // 无密码; token 已设
	_ = st.SaveOperator(op("ghost", store.RoleOperator, "tok-ghost")) // 无密码

	a := New(st, "")
	if id, tok, ok := a.Login("op", "s3cret"); !ok || id.Username != "op" || tok != "tok-op" {
		t.Fatalf("正确密码应登录成功, got id=%+v tok=%q ok=%v", id, tok, ok)
	}
	if _, _, ok := a.Login("op", "wrong"); ok {
		t.Fatal("错误密码不应通过")
	}
	if _, _, ok := a.Login("nogood", "x"); ok {
		t.Fatal("无密码操作员不应登录")
	}
	if _, _, ok := a.Login("ghost", "x"); ok {
		t.Fatal("无密码操作员不应登录")
	}
}

// TestContextIdentity 验证从请求头解析 + 写入 context。
func TestContextIdentity(t *testing.T) {
	st := store.New()
	_ = st.SaveOperator(op("bob", store.RoleOperator, "tok-bob"))

	a := New(st, "")
	req := httptest.NewRequest("GET", "/", nil)
	req.Header.Set("Authorization", "Bearer tok-bob")
	id, ok := a.ContextIdentity(req)
	if !ok || id.Username != "bob" {
		t.Fatalf("请求头解析失败: %+v ok=%v", id, ok)
	}

	req2 := httptest.NewRequest("GET", "/", nil)
	if _, ok := a.ContextIdentity(req2); ok {
		t.Fatal("无 token 不应通过")
	}
}
