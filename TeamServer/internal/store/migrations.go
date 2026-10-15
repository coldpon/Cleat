// goose 嵌入式迁移接线(第 4 批借壳, )。
// 迁移文件: migrations/ 目录(embed), SQL 迁移 + Go 迁移(本包注册)。
// 版本记账: goose_db_version 表(goose 自管) —— 启动即知 schema 新旧, 不再猜。
package store

import (
	"context"
	"database/sql"
	"embed"
	"sync"

	"github.com/pressly/goose/v3"
)

//go:embed migrations/*.sql
var migrationsFS embed.FS

// goose 全局状态一次性配置(v3.28 的经典 API 走全局: 方言/BaseFS/Go 迁移注册表)。
// **防重蹈覆辙**: ① 方言必须先设 —— 缺省时 goose 内部版本查询生成错误方言 SQL
// (实测报 near "BY" syntax error); ② Go 迁移注册表是全局的, 重复注册同 version 直接
// panic(测试进程多次 gooseUp 时触发) —— Once 化。
var gooseSetupOnce sync.Once

func setupGoose() {
	gooseSetupOnce.Do(func() {
		if err := goose.SetDialect(string(goose.DialectSQLite3)); err != nil {
			panic("goose 方言设置失败: " + err.Error()) // 编译期常量, 不可能失败
		}
		goose.SetBaseFS(migrationsFS)
		goose.AddNamedMigrationContext("00002_backfill_columns.go", upBackfillColumns, downBackfillColumns)
		goose.SetLogger(goose.NopLogger()) // 启动日志走 logx 摘要, 不用 goose 内建输出
	})
}

// gooseUp 执行全部待跑迁移。经典 API(SetBaseFS/UpContext): v3 全系可用,
// Provider 新 API 对 Go 迁移收集机制文档不足, 不冒险。
// 返回当前 schema 版本(诊断用: logx 打一行, schema 新旧一眼可见)。
func gooseUp(ctx context.Context, db *sql.DB) (int64, error) {
	setupGoose()
	if err := goose.UpContext(ctx, db, "migrations"); err != nil {
		return 0, err
	}
	v, _ := goose.GetDBVersionContext(ctx, db)
	return v, nil
}
