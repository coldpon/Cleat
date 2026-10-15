package store

import (
	"path/filepath"
	"testing"
)

// TestOperatorCRUD 验证操作员增改查删 + 角色写入判定。
func TestOperatorCRUD(t *testing.T) {
	st := New()

	if err := st.SaveOperator(&Operator{Username: "alice", Role: RoleAdmin, Token: "t-admin"}); err != nil {
		t.Fatalf("SaveOperator admin: %v", err)
	}
	if err := st.SaveOperator(&Operator{Username: "bob", Role: RoleOperator, Token: "t-bob", Active: true}); err != nil {
		t.Fatalf("SaveOperator operator: %v", err)
	}
	// 非法: 空用户名 / 不可识别角色 → 拒绝
	if err := st.SaveOperator(&Operator{Username: "", Role: RoleAdmin, Token: "x"}); err == nil {
		t.Fatal("空用户名应被拒绝")
	}
	if err := st.SaveOperator(&Operator{Username: "x", Role: "root", Token: "y"}); err == nil {
		t.Fatal("非法角色应被拒绝")
	}

	if o, ok := st.Operator("bob"); !ok || o.Role != RoleOperator {
		t.Fatalf("bob 应存在且为 operator: %+v", o)
	}
	if !RoleOperator.CanWrite() {
		t.Fatal("operator 应可写")
	}
	if RoleViewer.CanWrite() {
		t.Fatal("viewer 不应可写")
	}

	if o, ok := st.OperatorByToken("t-admin"); !ok || o.Username != "alice" {
		t.Fatalf("按 token 应匹配 alice, got %+v", o)
	}
	if _, ok := st.OperatorByToken(""); ok {
		t.Fatal("空 token 不应匹配")
	}
}

// TestSQLiteOperatorPersist 验证操作员落盘 + 重启恢复。
func TestSQLiteOperatorPersist(t *testing.T) {
	dbPath := filepath.Join(t.TempDir(), "op.db")
	st, err := WithSQLite(dbPath)
	if err != nil {
		t.Fatalf("WithSQLite: %v", err)
	}
	if err := st.SaveOperator(&Operator{Username: "ops", Role: RoleOperator, Token: "tok-op", Active: true}); err != nil {
		t.Fatalf("SaveOperator: %v", err)
	}
	if err := st.SaveOperator(&Operator{Username: "view", Role: RoleViewer, Token: "tok-v", Active: true}); err != nil {
		t.Fatalf("SaveOperator viewer: %v", err)
	}
	if err := st.SaveOperator(&Operator{Username: "pw", Role: RoleOperator, Token: "tok-pw", PasswordHash: "$2a$10$fakehash", Active: true}); err != nil {
		t.Fatalf("SaveOperator pw: %v", err)
	}
	_ = st.Close()

	st2, err := WithSQLite(dbPath)
	if err != nil {
		t.Fatalf("reopen: %v", err)
	}
	defer func() { _ = st2.Close() }()

	ops := st2.Operators()
	if len(ops) != 3 {
		t.Fatalf("重启应恢复 3 个操作员, got %d", len(ops))
	}
	if o, ok := st2.Operator("pw"); !ok || o.PasswordHash != "$2a$10$fakehash" {
		t.Fatalf("password_hash 应持久化恢复, got %+v", o)
	}
	if o, ok := st2.OperatorByToken("tok-op"); !ok || o.Username != "ops" {
		t.Fatalf("按 token 应匹配恢复的操作员, got %+v", o)
	}
}
