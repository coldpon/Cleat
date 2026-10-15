// 配置文件支持(server.toml, 业界 Mythic 式): 一段覆盖全部启动参数。
// 优先级: 内置默认 < server.toml < flag(-config 或旧 flag 显式)。缺失字段用默认, 不报错。
package config

import (
	"bytes"
	"fmt"
	"os"

	"github.com/BurntSushi/toml"
)

// fileConfig server.toml 的扁平映射, 段名映射到 Config 字段。
type fileConfig struct {
	Server struct {
		Listen string `toml:"listen"`
		API    string `toml:"api"`
		DB     string `toml:"db"`
		Token  string `toml:"token"`
		CLI    *bool  `toml:"cli"`
		Mimic  *bool  `toml:"mimic"`
	} `toml:"server"`
	Crypto struct {
		Encrypt   *bool  `toml:"encrypt"`
		PSK       string `toml:"psk"`
		X25519Key string `toml:"x25519_key"`
	} `toml:"crypto"`
	TLS struct {
		Enabled *bool  `toml:"enabled"`
		Cert    string `toml:"cert"`
		Key     string `toml:"key"`
	} `toml:"tls"`
	Admin struct {
		User     string `toml:"user"`
		Token    string `toml:"token"`
		Password string `toml:"password"`
	} `toml:"admin"`
	Beacon struct {
		Listen     string `toml:"listen"`
		PollPath   string `toml:"poll_path"`
		UploadPath string `toml:"upload_path"`
		TLSCert    string `toml:"tls_cert"`
		TLSKey     string `toml:"tls_key"`
		WSPath     string `toml:"ws_path"`
		QuicListen string `toml:"quic_listen"`
	} `toml:"beacon"`
	Artifact struct {
		OutRoot      string `toml:"out_root"`
		StubDir      string `toml:"stub_dir"`
		AllowCompile *bool  `toml:"allow_compile"`
	} `toml:"artifact"`
}

// CLIEnabled 是否启用交互 CLI(toml 特有, 供 main 用)。
func (c Config) CLIEnabled() bool { return c.CLI }

// LoadFromFile 从 server.toml 读取覆盖到 dst(缺失字段保留 dst 原值)。
// 文件不存在返回 nil(不报错), 由调用方决定是否提示。
func LoadFromFile(dst *Config, path string) error {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil // 无配置文件: 用默认
	}
	var f fileConfig
	if err := toml.Unmarshal(data, &f); err != nil {
		return fmt.Errorf("解析 %s 失败: %w", path, err)
	}
	if f.Server.Listen != "" {
		dst.ListenAddr = f.Server.Listen
	}
	if f.Server.API != "" {
		dst.APIServer = f.Server.API
	}
	if f.Server.DB != "" {
		dst.DBPath = f.Server.DB
	}
	if f.Server.Token != "" {
		dst.Token = f.Server.Token
	}
	if f.Server.CLI != nil {
		dst.CLI = *f.Server.CLI
	}
	if f.Server.Mimic != nil {
		dst.MimicEnabled = *f.Server.Mimic
	}
	if f.Crypto.Encrypt != nil {
		dst.Encryption = *f.Crypto.Encrypt
	}
	if f.Crypto.PSK != "" {
		dst.PSKHex = f.Crypto.PSK
	}
	if f.Crypto.X25519Key != "" {
		dst.X25519Key = f.Crypto.X25519Key
	}
	if f.TLS.Enabled != nil {
		dst.TLSEnabled = *f.TLS.Enabled
	}
	if f.TLS.Cert != "" {
		dst.TLSCert = f.TLS.Cert
	}
	if f.TLS.Key != "" {
		dst.TLSKey = f.TLS.Key
	}
	if f.Admin.User != "" {
		dst.AdminUser = f.Admin.User
	}
	if f.Admin.Token != "" {
		dst.AdminToken = f.Admin.Token
	}
	if f.Admin.Password != "" {
		dst.AdminPassword = f.Admin.Password
	}
	if f.Beacon.Listen != "" {
		dst.BeaconListen = f.Beacon.Listen
	}
	if f.Beacon.PollPath != "" {
		dst.BeaconURL = f.Beacon.PollPath
	}
	if f.Beacon.UploadPath != "" {
		dst.BeaconUpload = f.Beacon.UploadPath
	}
	if f.Beacon.TLSCert != "" {
		dst.BeaconTLSCert = f.Beacon.TLSCert
	}
	if f.Beacon.TLSKey != "" {
		dst.BeaconTLSKey = f.Beacon.TLSKey
	}
	if f.Beacon.WSPath != "" {
		dst.WSBeaconPath = f.Beacon.WSPath
	}
	if f.Beacon.QuicListen != "" {
		dst.QUICListen = f.Beacon.QuicListen
	}
	if f.Artifact.OutRoot != "" {
		dst.ArtifactOutRoot = f.Artifact.OutRoot
	}
	if f.Artifact.StubDir != "" {
		dst.ArtifactStubDir = f.Artifact.StubDir
	}
	if f.Artifact.AllowCompile != nil {
		dst.ArtifactAllowCompile = *f.Artifact.AllowCompile
	}
	return nil
}

// Template 返回带注释的 server.toml 模板(供 -config-init 输出)。
func Template() string {
	return `# Cleat TeamServer 配置(Mythic 式)。缺失字段用内置默认, 不强制手写全量。
# 优先级: 内置默认 < server.toml < 命令行 flag。
# 启动: teamserver.exe [-config server.toml]

[server]
listen   = "127.0.0.1:4444"   # Agent TCP 监听地址
api      = "127.0.0.1:8080"    # HTTP API / Web 监听地址
db       = "cleat.db"          # SQLite 路径, 空=""=纯内存
token    = ""                  # API bearer 兜底令牌(空=未配, 走操作员)
cli      = false               # true=同时启用交互式操作员 CLI

[crypto]                        # 传输加密, PSK 与 x25519_key 二选一
encrypt    = true               # 启用 XChaCha20-Poly1305 传输加密
psk        = ""                 # 预共享密钥(64位 hex); 与 Agent 一致
x25519_key = ""                 # X25519 长期私钥(64位 hex); 非空优先(握手式, 前向保密)

[tls]                           # HTTPS (Web/CLI 管理端口)
enabled = true                  # 启用 HTTPS
cert    = ""                     # TLS 证书路径; 空=自动生成自签证书
key     = ""                     # TLS 私钥路径

[admin]                         # 首次启动种子管理员(Web/CLI 登录)
user     = "admin"
token    = ""                   # 留空=自动生成随机 API token(登录返回)
password = ""                   # Web 登录密码 —— 空=与 token 皆空时不创建种子管理员
                                # 

[beacon]                        # 可选: HTTPS 多传输 listener(Agent --transport https)
listen      = ""                # 例 "127.0.0.1:8443"; 空=不启 beacon
poll_path   = "/beacon/poll"
upload_path = "/beacon/upload"
tls_cert    = ""                # 自签证书路径; 空=纯 HTTP
tls_key     = ""
ws_path     = ""                # wss 通道路径(骑本 listener; 例 "/ws"); 空=不启用

[artifact]                      # 样本生成器(模板烧录, 零工具链出样本; T1/T2)
out_root      = ""              # 产物根; 空=<db 目录>/artifacts(db 空则 ./artifacts)
stub_dir      = ""              # 外部 stub 目录覆盖; 空=仅用内置(go:embed)stub
allow_compile = false           # 现场编译路径(高级, 需 MSVC/cmake); 常规出样本走模板烧录
`
}

// FormatTOML 把当前 Config 输出为 server.toml 文本(供持久化当前配置)。
func FormatTOML(c Config) string {
	var buf bytes.Buffer
	enc := toml.NewEncoder(&buf)
	_ = enc.Encode(map[string]interface{}{
		"server": map[string]interface{}{
			"listen": c.ListenAddr, "api": c.APIServer, "db": c.DBPath,
			"token": c.Token, "cli": c.CLI, "mimic": c.MimicEnabled,
		},
		"crypto": map[string]interface{}{
			"encrypt": c.Encryption, "psk": c.PSKHex, "x25519_key": c.X25519Key,
		},
		"tls": map[string]interface{}{
			"enabled": c.TLSEnabled, "cert": c.TLSCert, "key": c.TLSKey,
		},
		"admin": map[string]interface{}{
			"user": c.AdminUser, "token": c.AdminToken, "password": c.AdminPassword,
		},
		"beacon": map[string]interface{}{
			"listen": c.BeaconListen, "poll_path": c.BeaconURL, "upload_path": c.BeaconUpload,
			"tls_cert": c.BeaconTLSCert, "tls_key": c.BeaconTLSKey, "ws_path": c.WSBeaconPath,
			"quic_listen": c.QUICListen,
		},
		"artifact": map[string]interface{}{
			"out_root": c.ArtifactOutRoot, "stub_dir": c.ArtifactStubDir,
			"allow_compile": c.ArtifactAllowCompile,
		},
	})
	return buf.String()
}
