-- +goose Up
-- 00003: 会话通道类型(transport)列 —— Web 会话列表"通道"列的真实数据来源。
-- 取值: tcp | https | http | dns | ws | pipe; 由各连接入口在会话建立时写入。
-- ADD COLUMN ... DEFAULT 会把存量行一并填成 'tcp'(历史会话绝大多数走 TCP),
-- 新建会话由代码显式覆盖为真实通道, 不依赖本默认值。
ALTER TABLE sessions ADD COLUMN transport TEXT DEFAULT 'tcp';

-- +goose Down
-- 补列不回滚(SQLite 3.35 前无 DROP COLUMN; 缺列只有"更旧版本"需要)。
