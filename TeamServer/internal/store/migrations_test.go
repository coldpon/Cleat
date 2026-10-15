package store

// goose 迁移测试(第 4 批, ): 锁定"全新库一步建全"与"历史旧库升级"两条路径。
// 旧库形态 = 历史上"吞错 ALTER"之前的原始 CREATE(缺 pid/process_name/privilege/password_hash)。

import (
	"context"
	"database/sql"
	"path/filepath"
	"testing"

	_ "modernc.org/sqlite"
)

// TestMigrateFreshDB 全新库: goose Up 一次建全(7 表 + goose_db_version 记账)。
func TestMigrateFreshDB(t *testing.T) {
	db, err := sql.Open("sqlite", filepath.Join(t.TempDir(), "fresh.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer func() { _ = db.Close() }()
	if _, err := gooseUp(context.Background(), db); err != nil {
		t.Fatalf("gooseUp: %v", err)
	}
	for _, tbl := range []string{"sessions", "audit_log", "tasks", "operators", "credentials", "targets", "events", "goose_db_version"} {
		var n int
		if err := db.QueryRow("SELECT count(*) FROM sqlite_master WHERE type='table' AND name=?", tbl).Scan(&n); err != nil || n != 1 {
			t.Fatalf("表 %s 应存在, err=%v n=%d", tbl, err, n)
		}
	}
	var ver int64
	if err := db.QueryRow("SELECT max(version_id) FROM goose_db_version").Scan(&ver); err != nil || ver < 2 {
		t.Fatalf("版本应 ≥2, got %d err=%v", ver, err)
	}
}

// TestMigrateLegacyDB 历史旧库升级: 停在"吞错 ALTER 之前"形态的库 → Up 后补齐缺列,
// 既有数据保留。
func TestMigrateLegacyDB(t *testing.T) {
	db, err := sql.Open("sqlite", filepath.Join(t.TempDir(), "legacy.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer func() { _ = db.Close() }()
	// 手工造 V0 旧库: 老形态 sessions(无 pid/process_name/privilege) + operators(无 password_hash),
	// 各带一行存量数据
	for _, stmt := range []string{
		`CREATE TABLE sessions (id INTEGER PRIMARY KEY, machine_id TEXT, hostname TEXT, os TEXT, os_version TEXT, cpu TEXT, memory TEXT, local_ip TEXT, arch TEXT, last_seen INTEGER)`,
		`CREATE TABLE operators (username TEXT PRIMARY KEY, role TEXT, token TEXT, active INTEGER, created_at INTEGER)`,
		`INSERT INTO sessions (id, machine_id, hostname) VALUES (7, 'legacy-mid', 'legacy-host')`,
		`INSERT INTO operators (username, role, token, active) VALUES ('admin', 'admin', 'tok', 1)`,
	} {
		if _, err := db.Exec(stmt); err != nil {
			t.Fatalf("造旧库: %v", err)
		}
	}

	if _, err := gooseUp(context.Background(), db); err != nil {
		t.Fatalf("旧库升级: %v", err)
	}
	// 缺列已补齐
	var pid int
	if err := db.QueryRow("SELECT pid FROM sessions WHERE id=7").Scan(&pid); err != nil {
		t.Fatalf("sessions.pid 应已补齐: %v", err)
	}
	var hash sql.NullString
	if err := db.QueryRow("SELECT password_hash FROM operators WHERE username='admin'").Scan(&hash); err != nil {
		t.Fatalf("operators.password_hash 应已补齐: %v", err)
	}
	// 存量数据完好
	var mid string
	if err := db.QueryRow("SELECT machine_id FROM sessions WHERE id=7").Scan(&mid); err != nil || mid != "legacy-mid" {
		t.Fatalf("存量数据应保留, got %q err=%v", mid, err)
	}
	// 再跑一遍: 幂等(已是最新版本, no-op)
	if _, err := gooseUp(context.Background(), db); err != nil {
		t.Fatalf("重复 Up 应幂等: %v", err)
	}
}

// TestWithSQLiteEndToEnd 全链路: WithSQLite 打开 → 迁移自动完成 → 正常读写。
func TestWithSQLiteEndToEnd(t *testing.T) {
	st, err := WithSQLite(filepath.Join(t.TempDir(), "e2e.db"))
	if err != nil {
		t.Fatalf("WithSQLite: %v", err)
	}
	defer func() { _ = st.Close() }() // Windows: 不关句柄 TempDir 清理会 unlinkat 失败
	id, err := st.SaveTask(&Task{SessionID: 1, Cmd: "X", Mode: "push", Status: TaskPending})
	if err != nil {
		t.Fatalf("SaveTask: %v", err)
	}
	if tk, ok := st.GetTask(id); !ok || tk.Cmd != "X" {
		t.Fatalf("任务应可读写, got %+v ok=%v", tk, ok)
	}
}
