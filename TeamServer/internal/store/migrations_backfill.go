// Package store 迁移: goose 嵌入式(embed), 由 store_persist.go 的 WithSQLite 驱动。
// 迁移铁律: 00001 基线后一切 schema 变更加新文件、永不改历史文件;
// **严禁**回到"吞错 ALTER 当版本管理"的旧路(store_persist.go:69 曾靠
// `_, _ = db.Exec(ALTER...)` 忽略错误幂等 —— 错误被吞的同时真正的失败也被吞)。
package store

import (
	"context"
	"database/sql"
	"fmt"
)

// upBackfillColumns 00002: 历史旧库停在任何"吞错 ALTER"版本之前的, 补齐缺列。
// 全新库/近期库(V1 建全列) → 探测后全部命中, no-op。
// 防重蹈覆辙: ALTER 前必须探测 —— SQLite 无 ADD COLUMN IF NOT EXISTS,
// 盲执行对已升级库必然报错, goose 会把迁移标失败中止。
// goose v3.28 Go 迁移在事务内执行(签名 *sql.Tx): PRAGMA 读 + ALTER DDL 同事务合法。
func upBackfillColumns(ctx context.Context, tx *sql.Tx) error {
	type add struct {
		table, column, decl string
	}
	wants := []add{
		{"sessions", "pid", "INTEGER DEFAULT 0"},
		{"sessions", "process_name", "TEXT DEFAULT ''"},
		{"sessions", "privilege", "TEXT DEFAULT ''"},
		{"operators", "password_hash", "TEXT"},
	}
	for _, w := range wants {
		rows, err := tx.QueryContext(ctx, fmt.Sprintf("PRAGMA table_info(%s)", w.table))
		if err != nil {
			return fmt.Errorf("探测 %s 列失败: %w", w.table, err)
		}
		cols := map[string]bool{}
		for rows.Next() {
			var cid, notNull, pk int
			var name, ctype string
			var dflt sql.NullString
			if err := rows.Scan(&cid, &name, &ctype, &notNull, &dflt, &pk); err != nil {
				_ = rows.Close()
				return err
			}
			cols[name] = true
		}
		_ = rows.Close()
		if cols[w.column] {
			continue
		}
		if _, err := tx.ExecContext(ctx,
			fmt.Sprintf("ALTER TABLE %s ADD COLUMN %s %s", w.table, w.column, w.decl)); err != nil {
			return fmt.Errorf("补列 %s.%s 失败: %w", w.table, w.column, err)
		}
	}
	return nil
}

func downBackfillColumns(_ context.Context, _ *sql.Tx) error {
	// 补列不回滚: SQLite 3.35 前 DROP COLUMN 不可用, 且缺列只有"更旧版本"需要。
	return nil
}
