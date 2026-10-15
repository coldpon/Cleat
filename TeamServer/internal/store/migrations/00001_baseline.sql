-- +goose Up
-- 00001 基线(第 4 批借壳 goose, 2026-09-28): 现状 schema 原文收编。
-- 幂等设计(CREATE IF NOT EXISTS): 对既有旧库首跑安全(表已存在则跳过),
-- 对全新库一步建全(含历史上"吞错 ALTER"追加的全部列)。
-- **版本化铁律**: 本文件入库后永不修改; 一切 schema 变更加新文件 000NN_*.sql。

CREATE TABLE IF NOT EXISTS sessions (
    id INTEGER PRIMARY KEY,
    machine_id TEXT,
    hostname TEXT, os TEXT, os_version TEXT, cpu TEXT,
    memory TEXT, local_ip TEXT, arch TEXT,
    last_seen INTEGER
);
CREATE TABLE IF NOT EXISTS audit_log (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    ts INTEGER, operator TEXT, session_id INTEGER,
    cmd TEXT, mode TEXT, status TEXT
);
CREATE TABLE IF NOT EXISTS tasks (
    id INTEGER PRIMARY KEY,
    session_id INTEGER,
    cmd TEXT,
    params TEXT,
    request_id TEXT,
    mode TEXT,
    status TEXT,
    result TEXT,
    created_at INTEGER,
    updated_at INTEGER
);
CREATE INDEX IF NOT EXISTS idx_tasks_session ON tasks(session_id);
CREATE INDEX IF NOT EXISTS idx_tasks_status ON tasks(status);
CREATE TABLE IF NOT EXISTS operators (
    username TEXT PRIMARY KEY,
    role TEXT,
    token TEXT,
    password_hash TEXT,
    active INTEGER,
    created_at INTEGER
);
CREATE TABLE IF NOT EXISTS credentials (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    username TEXT DEFAULT '',
    realm TEXT DEFAULT '',
    cred_type TEXT DEFAULT 'password',
    secret TEXT DEFAULT '',
    host TEXT DEFAULT '',
    source TEXT DEFAULT '',
    agent_id INTEGER DEFAULT 0,
    tag TEXT DEFAULT '',
    created_at INTEGER DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_creds_host ON credentials(host);
CREATE INDEX IF NOT EXISTS idx_creds_type ON credentials(cred_type);
CREATE TABLE IF NOT EXISTS targets (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    ip TEXT DEFAULT '',
    hostname TEXT DEFAULT '',
    os TEXT DEFAULT '',
    ports TEXT DEFAULT '',
    protocol TEXT DEFAULT 'tcp',
    status TEXT DEFAULT 'new',
    tags TEXT DEFAULT '',
    notes TEXT DEFAULT '',
    agent_id INTEGER DEFAULT 0,
    created_at INTEGER DEFAULT 0,
    updated_at INTEGER DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_targets_ip ON targets(ip);
CREATE INDEX IF NOT EXISTS idx_targets_status ON targets(status);
CREATE TABLE IF NOT EXISTS events (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    ts INTEGER,
    seq INTEGER,
    type TEXT DEFAULT '',
    session_id INTEGER DEFAULT 0,
    data TEXT DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_events_ts ON events(ts);
CREATE INDEX IF NOT EXISTS idx_events_type ON events(type);
CREATE INDEX IF NOT EXISTS idx_events_session ON events(session_id);

-- +goose Down
DROP INDEX IF EXISTS idx_events_session; DROP INDEX IF EXISTS idx_events_type; DROP INDEX IF EXISTS idx_events_ts;
DROP TABLE IF EXISTS events;
DROP INDEX IF EXISTS idx_targets_status; DROP INDEX IF EXISTS idx_targets_ip;
DROP TABLE IF EXISTS targets;
DROP INDEX IF EXISTS idx_creds_type; DROP INDEX IF EXISTS idx_creds_host;
DROP TABLE IF EXISTS credentials;
DROP TABLE IF EXISTS operators;
DROP INDEX IF EXISTS idx_tasks_status; DROP INDEX IF EXISTS idx_tasks_session;
DROP TABLE IF EXISTS tasks;
DROP TABLE IF EXISTS audit_log;
DROP TABLE IF EXISTS sessions;
