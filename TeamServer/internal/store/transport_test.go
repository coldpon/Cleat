package store

// 会话通道类型(transport)持久化测试(Web 修复批 1-A, ):
// 锁定 00003_session_transport.sql 迁移 + SaveSession/LoadSessions 往返两条路径,
// 确保 Web 会话列表"通道"列的真实数据来源不被回归。

import (
	"context"
	"database/sql"
	"path/filepath"
	"testing"

	_ "modernc.org/sqlite"
)

// TestSessionTransportRoundTrip 建库 → 写入带 transport 的记录 → 重开库 → 读回应一致。
// 覆盖"新库建全 + SaveSession/LoadSessions 携带 transport"。
func TestSessionTransportRoundTrip(t *testing.T) {
	path := filepath.Join(t.TempDir(), "transport.db")
	st, err := WithSQLite(path)
	if err != nil {
		t.Fatalf("WithSQLite: %v", err)
	}
	st.Put(&Record{ID: 1, MachineID: "m-transport", Hostname: "h1", Transport: "dns"})
	st.Put(&Record{ID: 2, MachineID: "m-empty", Hostname: "h2"}) // 空 transport = 未知
	if err := st.Close(); err != nil {                           // Windows: 关句柄再重开
		t.Fatalf("Close: %v", err)
	}

	st2, err := WithSQLite(path)
	if err != nil {
		t.Fatalf("WithSQLite(重开): %v", err)
	}
	defer func() { _ = st2.Close() }()

	rec := st2.Get(1)
	if rec == nil {
		t.Fatal("记录 #1 应已从库中加载")
	}
	if rec.Transport != "dns" {
		t.Fatalf("transport 应往返一致, want \"dns\" got %q", rec.Transport)
	}
	if got := st2.Get(2); got == nil || got.Transport != "" {
		t.Fatalf("空 transport 应还原为空串, got %+v", got)
	}
}

// TestMigrateTransportColumnLegacy 历史旧库(无 transport 列)升级后:
// 列被补出, 存量行按 DEFAULT 填 'tcp'(历史会话绝大多数走 TCP), 存量数据保留。
func TestMigrateTransportColumnLegacy(t *testing.T) {
	db, err := sql.Open("sqlite", filepath.Join(t.TempDir(), "legacy-transport.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer func() { _ = db.Close() }()
	// V0 旧库: sessions 无 transport 列 + 一行存量数据
	for _, stmt := range []string{
		`CREATE TABLE sessions (id INTEGER PRIMARY KEY, machine_id TEXT, hostname TEXT, os TEXT, os_version TEXT, cpu TEXT, memory TEXT, local_ip TEXT, arch TEXT, last_seen INTEGER)`,
		`INSERT INTO sessions (id, machine_id, hostname) VALUES (7, 'legacy-mid', 'legacy-host')`,
	} {
		if _, err := db.Exec(stmt); err != nil {
			t.Fatalf("造旧库: %v", err)
		}
	}

	if _, err := gooseUp(context.Background(), db); err != nil {
		t.Fatalf("旧库升级: %v", err)
	}
	// transport 列已补齐, 存量行填 DEFAULT 'tcp'
	var tr sql.NullString
	if err := db.QueryRow("SELECT transport FROM sessions WHERE id=7").Scan(&tr); err != nil {
		t.Fatalf("sessions.transport 应已补齐: %v", err)
	}
	if !tr.Valid || tr.String != "tcp" {
		t.Fatalf("存量行 transport 应为 'tcp', got valid=%v %q", tr.Valid, tr.String)
	}
	// 存量数据完好
	var mid string
	if err := db.QueryRow("SELECT machine_id FROM sessions WHERE id=7").Scan(&mid); err != nil || mid != "legacy-mid" {
		t.Fatalf("存量数据应保留, got %q err=%v", mid, err)
	}
}
