// Package tlsutil 自签证书生成 + TLS 配置辅助。
// 首次启用 HTTPS 时自动生成 ECDSA P-256 自签证书, 存到 data 目录,
// 后续启动复用(证书有效期 10 年)。
package tlsutil

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/pem"
	"fmt"
	"math/big"
	"net"
	"os"
	"path/filepath"
	"time"
)

// EnsureCert 检查 cert/key 文件是否存在, 不存在则自动生成。
// 返回 (certPath, keyPath, error)。
func EnsureCert(dataDir string) (string, string, error) {
	certPath := filepath.Join(dataDir, "server.crt")
	keyPath := filepath.Join(dataDir, "server.key")

	// 两个文件都存在则直接返回
	if fileExists(certPath) && fileExists(keyPath) {
		return certPath, keyPath, nil
	}

	// 确保目录存在
	if err := os.MkdirAll(dataDir, 0o700); err != nil {
		return "", "", fmt.Errorf("创建证书目录失败: %w", err)
	}

	if err := generateCert(certPath, keyPath); err != nil {
		return "", "", err
	}
	return certPath, keyPath, nil
}

// CipherSuites 返回统一使用的现代密码套件白名单。
// 抽出为独立函数: 原仅有 TLSConfig 内联一份, 导致
// beacon 侧两条 TLS 路径(beacon.SelfSignedTLS / main.go beaconTLS)各写各的,
// 出现"一条限制套件、另一条完全不限制"的策略分裂。统一从此处取。
//
// 注意: 该白名单仅对 TLS 1.2 生效 —— TLS 1.3 的套件不可配置(Go 亦忽略该字段),
// 由 crypto/tls 内置的强套件集合固定处理。
func CipherSuites() []uint16 {
	return []uint16{
		tls.TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
		tls.TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
		tls.TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305,
		tls.TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
		tls.TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
	}
}

// TLSConfig 返回标准 TLS 配置(现代密码套件, 最低 TLS 1.2)。
func TLSConfig() *tls.Config {
	return &tls.Config{
		MinVersion:   tls.VersionTLS12,
		CipherSuites: CipherSuites(),
	}
}

func generateCert(certPath, keyPath string) error {
	// ECDSA P-256 私钥
	priv, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		return fmt.Errorf("生成私钥失败: %w", err)
	}

	// 随机序列号
	serial, err := rand.Int(rand.Reader, new(big.Int).Lsh(big.NewInt(1), 128))
	if err != nil {
		return fmt.Errorf("生成序列号失败: %w", err)
	}

	template := x509.Certificate{
		SerialNumber: serial,
		Subject: pkix.Name{
			Organization: []string{"Cleat TeamServer"},
			CommonName:   "Cleat C2",
		},
		NotBefore:             time.Now().Add(-1 * time.Hour),
		NotAfter:              time.Now().Add(10 * 365 * 24 * time.Hour), // 10 年
		KeyUsage:              x509.KeyUsageDigitalSignature | x509.KeyUsageKeyEncipherment,
		ExtKeyUsage:           []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
		BasicConstraintsValid: true,
	}

	// 自动添加 localhost 和常见本地地址
	template.IPAddresses = []net.IP{
		net.ParseIP("127.0.0.1"),
		net.ParseIP("::1"),
	}
	template.DNSNames = []string{"localhost"}

	// 自签证书
	certDER, err := x509.CreateCertificate(rand.Reader, &template, &template, &priv.PublicKey, priv)
	if err != nil {
		return fmt.Errorf("生成证书失败: %w", err)
	}

	// 写证书文件
	certFile, err := os.OpenFile(certPath, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, 0o644)
	if err != nil {
		return fmt.Errorf("写证书文件失败: %w", err)
	}
	defer func() { _ = certFile.Close() }()
	if err := pem.Encode(certFile, &pem.Block{Type: "CERTIFICATE", Bytes: certDER}); err != nil {
		return err
	}

	// 写私钥文件
	keyDER, err := x509.MarshalECPrivateKey(priv)
	if err != nil {
		return err
	}
	keyFile, err := os.OpenFile(keyPath, os.O_WRONLY|os.O_CREATE|os.O_TRUNC, 0o600)
	if err != nil {
		return fmt.Errorf("写私钥文件失败: %w", err)
	}
	defer func() { _ = keyFile.Close() }()
	if err := pem.Encode(keyFile, &pem.Block{Type: "EC PRIVATE KEY", Bytes: keyDER}); err != nil {
		return err
	}

	return nil
}

func fileExists(p string) bool {
	_, err := os.Stat(p)
	return err == nil
}
