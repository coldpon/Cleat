package config

import (
	"os"
	"path/filepath"
	"testing"
)

// TestLoadFromFile 验证 server.toml 覆盖默认字段(缺失字段保留默认)。
func TestLoadFromFile(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "server.toml")
	tomlText := `[server]
listen = "0.0.0.0:9000"
api = "0.0.0.0:9090"
db = "x.db"
cli = true

[crypto]
encrypt = true
psk = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

[admin]
user = "boss"
password = "secret"

[beacon]
listen = "127.0.0.1:8443"
`
	if err := os.WriteFile(p, []byte(tomlText), 0o644); err != nil {
		t.Fatal(err)
	}
	cfg := Default()
	if err := LoadFromFile(&cfg, p); err != nil {
		t.Fatalf("LoadFromFile: %v", err)
	}
	if cfg.ListenAddr != "0.0.0.0:9000" {
		t.Errorf("listen 未覆盖: %q", cfg.ListenAddr)
	}
	if cfg.DBPath != "x.db" {
		t.Errorf("db 未覆盖: %q", cfg.DBPath)
	}
	if !cfg.CLI {
		t.Error("cli=true 应覆盖")
	}
	if cfg.PSKHex != "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef" {
		t.Errorf("psk 未覆盖")
	}
	if cfg.AdminUser != "boss" || cfg.AdminPassword != "secret" {
		t.Errorf("admin 未覆盖: %+v", cfg)
	}
	if cfg.BeaconListen != "127.0.0.1:8443" {
		t.Errorf("beacon listen 未覆盖")
	}
	// MimicEnabled 未在 toml: 保留默认 true
	if !cfg.MimicEnabled {
		t.Error("未指定字段应保留默认")
	}
}

// TestLoadFromFileMissing 验证文件不存在返回 nil(不报错)。
func TestLoadFromFileMissing(t *testing.T) {
	cfg := Default()
	if err := LoadFromFile(&cfg, filepath.Join(t.TempDir(), "nope.toml")); err != nil {
		t.Fatalf("缺失文件应返回 nil, got %v", err)
	}
}

// TestTemplateRoundtrip 验证模板可被自身解析回默认(无报错)。
func TestTemplateRoundtrip(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "t.toml")
	if err := os.WriteFile(p, []byte(Template()), 0o644); err != nil {
		t.Fatal(err)
	}
	cfg := Default()
	if err := LoadFromFile(&cfg, p); err != nil {
		t.Fatalf("模板应可解析: %v", err)
	}
}
