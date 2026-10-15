package artifact

// 无需工具链: 全部走内存 PatchBytes + go:embed stub。

import (
	"bytes"
	"os"
	"strings"
	"testing"
)

// TestStubRegistryMagic 内置注册表至少含 1 个 stub(exe), 且每个 stub 均含配置块魔数。
func TestStubRegistryMagic(t *testing.T) {
	reg, err := NewStubRegistry("")
	if err != nil {
		t.Fatalf("NewStubRegistry: %v", err)
	}
	list := reg.List()
	if len(list) == 0 {
		t.Fatal("内置注册表应至少含 1 个 stub(exe)")
	}
	for _, info := range list {
		blob, got, err := reg.Get(info.Form)
		if err != nil {
			t.Fatalf("Get(%s): %v", info.Form, err)
		}
		if !bytes.Contains(blob, cfgMagic) {
			t.Errorf("stub %s 缺配置块魔数(非可烧录模板)", info.Form)
		}
		if info.Size != int64(len(blob)) {
			t.Errorf("stub %s 元数据 size=%d 与实际字节 %d 不符", info.Form, info.Size, len(blob))
		}
		if got.BlockVersion != cfgVersion {
			t.Errorf("stub %s block_version=%d, want %d", info.Form, got.BlockVersion, cfgVersion)
		}
	}
	// 未注册形态 → ErrStubNotFound
	if _, _, err := reg.Get("dll"); err != ErrStubNotFound {
		t.Errorf("未注册形态应返回 ErrStubNotFound, got %v", err)
	}
}

// TestPatchBytesRoundTrip 烧录 → 读回 JSON 与输入一致; 文件长度不变(= stub 长度);
// 入参(stub 字节)不被修改(内存版返回新副本)。
func TestPatchBytesRoundTrip(t *testing.T) {
	reg, err := NewStubRegistry("")
	if err != nil {
		t.Fatalf("NewStubRegistry: %v", err)
	}
	stub, _, err := reg.Get("exe")
	if err != nil {
		t.Fatalf("Get(exe): %v", err)
	}
	off := bytes.Index(stub, cfgMagic)
	if off < 0 {
		t.Fatal("stub 应含配置块魔数")
	}
	cfg := map[string]any{
		"transport":     "tcp",
		"host":          "1.2.3.4",
		"port":          "4444",
		"server_pubkey": strings.Repeat("ab", 32),
		"recovery":      map[string]any{"key": strings.Repeat("cd", 32)},
	}
	out, err := PatchBytes(stub, cfg)
	if err != nil {
		t.Fatalf("PatchBytes: %v", err)
	}
	if len(out) != len(stub) {
		t.Fatalf("patch 不应改变文件长度: got %d want %d", len(out), len(stub))
	}
	if out[off+17] != cfgStateSet {
		t.Errorf("烧录后状态位应为 %d, got %d", cfgStateSet, out[off+17])
	}
	// 入参未被就地修改(原 stub 状态位仍为 0=模板)
	if stub[off+17] != 0 {
		t.Errorf("PatchBytes 不应修改入参: stub 状态位=%d", stub[off+17])
	}

	got, err := DecodeConfigBlock(out)
	if err != nil {
		t.Fatalf("DecodeConfigBlock: %v", err)
	}
	for _, k := range []string{"transport", "host", "port", "server_pubkey"} {
		if gv, _ := got[k].(string); gv != cfg[k] {
			t.Errorf("字段 %s 往返不一致: got %q want %v", k, gv, cfg[k])
		}
	}
	rec, ok := got["recovery"].(map[string]any)
	want := cfg["recovery"].(map[string]any)["key"]
	if !ok || rec["key"] != want {
		t.Errorf("recovery.key 往返不一致: got %v want %v", got["recovery"], want)
	}
}

// TestPatchBytesTooLarge 载荷 > cfgBlobSize-cfgHeadLen(16364) → 报错; 略小 → 成功; 无魔数 → 报错。
func TestPatchBytesTooLarge(t *testing.T) {
	// 合成一个含魔数、总长 ≥ cfgBlobSize 的模板(魔数置于偏移 0)
	tmpl := append(append([]byte{}, cfgMagic...), make([]byte, cfgBlobSize)...)

	tooBig := map[string]any{"blob": strings.Repeat("x", cfgBlobSize-cfgHeadLen)}
	if _, err := PatchBytes(tmpl, tooBig); err == nil {
		t.Fatal("载荷超上限应报错")
	}
	// 归入边界内(留出 JSON 结构开销的余量)
	ok := map[string]any{"blob": strings.Repeat("x", cfgBlobSize-cfgHeadLen-64)}
	if _, err := PatchBytes(tmpl, ok); err != nil {
		t.Fatalf("未超限应成功: %v", err)
	}
	// 无魔数模板
	if _, err := PatchBytes(make([]byte, 64), map[string]any{"a": "b"}); err == nil {
		t.Fatal("无魔数模板应报错")
	}
}

// TestValidatePatchCfg A 层条件必填: 各 transport 缺字段 → 错误; 齐全 → nil。
func TestValidatePatchCfg(t *testing.T) {
	good := []map[string]any{
		{"transport": "tcp", "host": "1.2.3.4", "port": "4444"},
		{"transport": "tcp", "host": "1.2.3.4", "port": "65535"}, // D.12 边界
		{"transport": "https", "url": "https://c2/beacon/poll"},
		{"transport": "dns", "dns_server": "1.2.3.4:53", "dns_domain": "c2.example.com"},
		{"transport": "icmp", "icmp_target": "1.2.3.4"},
		{"transport": "wss", "wss_url": "wss://c2/ws"},
	}
	for i, c := range good {
		if err := ValidatePatchCfg(c); err != nil {
			t.Errorf("good[%d] 应通过: %v", i, err)
		}
	}
	bad := []map[string]any{
		{"transport": "tcp", "host": "1.2.3.4"},                  // 缺 port
		{"transport": "tcp", "port": "4444"},                     // 缺 host
		{"transport": "tcp", "host": "1.2.3.4", "port": "70000"}, // D.12 越界
		{"transport": "tcp", "host": "1.2.3.4", "port": "0"},     // D.12 越界
		{"transport": "https"},                           // 缺 url
		{"transport": "dns", "dns_server": "1.2.3.4:53"}, // 缺 dns_domain
		{"transport": "icmp"},                            // 缺 icmp_target
		{"transport": "wss"},                             // 缺 wss_url
		{"transport": "pipe"},                            // 未知模式
		{},                                               // 空 transport
	}
	for i, c := range bad {
		if err := ValidatePatchCfg(c); err == nil {
			t.Errorf("bad[%d] 应报错: %v", i, c)
		}
	}
}

// TestWriteFileAtomic 原子写: 落盘内容正确; 临时文件不残留。
func TestWriteFileAtomic(t *testing.T) {
	dir := t.TempDir()
	dst := dir + "/out.bin"
	if err := WriteFileAtomic(dst, []byte("hello"), 0o600); err != nil {
		t.Fatalf("WriteFileAtomic: %v", err)
	}
	data, err := os.ReadFile(dst)
	if err != nil {
		t.Fatalf("读取: %v", err)
	}
	if string(data) != "hello" {
		t.Errorf("内容不符: %q", data)
	}
	if _, err := os.Stat(dst + ".tmp"); !os.IsNotExist(err) {
		t.Errorf("临时文件应已清除: %v", err)
	}
}
