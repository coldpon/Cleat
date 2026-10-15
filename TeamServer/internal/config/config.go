// Package config 集中管理 TeamServer 配置(监听端口/加密 PSK/mimic 开关)。
// 业界惯例: 配置从 flag/env/config 文件注入, 不在代码里硬编码默认密钥。
package config

import (
	"fmt"
	"strings"

	"cleat/teamserver/internal/logx"
)

// Config 是 TeamServer 的运行时配置。
type Config struct {
	// ListenAddr 植入物 TCP 监听地址(默认 :4444)
	ListenAddr string
	// APIServer HTTP 监听地址(默认 :8080, 供 Web/CLI)
	APIServer string
	// DBPath SQLite 数据库路径(空=纯内存)
	DBPath string
	// Token HTTP API 访问令牌(bearer; 空=无认证, 仅开发)
	Token string
	// Encryption 是否启用传输加密(XChaCha20-Poly1305)
	Encryption bool
	// PSKHex 预共享密钥(64位 hex); Encryption=true 且未给 X25519Key 时必填
	PSKHex string
	// X25519Key X25519 长期私钥(64位 hex); 非空时 beacon 走 X25519 握手模式(与 PSK 二选一)
	X25519Key string
	// MimicEnabled 帧级 HTTP 伪装壳(与 Agent CLEAT_HTTP_MIMIC 对应)
	MimicEnabled bool
	// AdminUser/AdminToken/AdminPassword 首次启动种子管理员操作员(空=不创建)
	AdminUser     string
	AdminToken    string
	AdminPassword string
	// BeaconHTTPS 启用时; ListenAddr 是 beacon HTTP(S) 监听地址
	BeaconListen  string
	BeaconURL     string // GET 拉取路径
	BeaconUpload  string // POST 上传路径
	BeaconTLSCert string // 可选 TLS 证书路径(自签; 空 = 纯 HTTP)
	BeaconTLSKey  string
	// WSBeaconPath WebSocket beacon 路径(wss 通道; 骑 BeaconListen 的 TLS 监听,
	WSBeaconPath string
	// DNSListen/DNSDomain DNS(beacon) 监听: 迷你 DNS 服务器(UDP), 需同时给查询域
	// (与 Agent `--dns-server`/`--domain` 两端一致)。空 = 不启动
	DNSListen string
	DNSDomain string
	// QUICListen QUIC(beacon) 监听地址(UDP; 如 ":4433")。空 = 不启动。
	// quic-go 自持 socket + 自签 TLS(ALPN=cleat-quic); 桥接后与 TCP/WS 同族,
	// 与 HTTPS/DNS 共用会话层(见 beacon/quic.go)。
	QUICListen string
	// CLI 是否启用交互式操作员 CLI(server.toml 的 cli 字段)
	CLI bool
	// TLSEnabled 启用 HTTPS(API 端口走 TLS)
	TLSEnabled bool
	// TLSCert/TLSKey TLS 证书/私钥路径(空=自签证书自动生成)
	TLSCert string
	TLSKey  string
	// ArtifactOutRoot 产物根; 空 = <DBPath 目录>/artifacts(DBPath 空则 ./artifacts)
	ArtifactOutRoot string
	// ArtifactStubDir 外部 stub 目录覆盖; 空 = 仅用 go:embed 内置
	ArtifactStubDir string
	// ArtifactAllowCompile 是否启用重量级现场编译路径(默认 false; 见 artifact.Generate)
	ArtifactAllowCompile bool
}

// Default 返回默认配置(开发用; PSK 由调用方注入, 不在代码硬编码)。
func Default() Config {
	return Config{
		ListenAddr:   ":4444",
		APIServer:    ":8080",
		DBPath:       "",
		Encryption:   true,
		PSKHex:       "",
		MimicEnabled: true,
		BeaconURL:    "/beacon/poll",
		BeaconUpload: "/beacon/upload",
	}
}

// Validate 校验配置合法性,返回错误描述(空=合法)。
func (c Config) Validate() error {
	if c.Encryption && c.X25519Key == "" && len(c.PSKHex) != 64 {
		return fmt.Errorf("启用加密时需提供 -psk(64 位 hex) 或 -x25519-key; 也可在 server.toml 的 [crypto] 段配置")
	}
	if c.DNSListen != "" && strings.Trim(c.DNSDomain, ".") == "" {
		return fmt.Errorf("启用 -dns-listen 时需同时提供 -dns-domain(查询域, 与 Agent --domain 一致)")
	}
	if c.WSBeaconPath != "" && c.BeaconListen == "" {
		return fmt.Errorf("启用 -ws-path 时需同时提供 -beacon-listen(wss 骑 beacon TLS 监听, WebSocket 通道设计 D9)")
	}
	// A1-7(TS-5): 模板默认口令残留 —— 警告不阻断(本地 dev 可用; 公网部署前必须改密)
	if c.AdminPassword == "admin123" {
		logx.Printf("[!] 安全提示 管理员口令为默认值 admin123，生产部署前请修改\n")
	}
	return nil
}
