package service

// 全程不触发编译: 产物字节 = 内置 stub 仅配置块区域被改写。

import (
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"sync"
	"testing"

	"cleat/teamserver/internal/artifact"
	"cleat/teamserver/internal/eventbus"
	"cleat/teamserver/internal/session"
	"cleat/teamserver/internal/store"
)

// newPayloadService 构造一个已装配产物环境(产物根=outRoot)的服务实例。
func newPayloadService(t *testing.T, outRoot string) *Service {
	t.Helper()
	st := store.New()
	bus := eventbus.New()
	mgr := session.NewManager(st, bus, nil, nil, false)
	svc := New(mgr, st, nil, "")
	if err := svc.SetArtifact(outRoot, "", false); err != nil {
		t.Fatalf("SetArtifact: %v", err)
	}
	return svc
}

// TestGeneratePayloadNoToolchain 生成成功且**未发生任何编译**(零工具链):
// 产物与内置 stub 长度一致, 且差异只落在 16KB 配置块窗口内 —— 若非纯烧录(走了编译)必然破坏此性质。
func TestGeneratePayloadNoToolchain(t *testing.T) {
	out := t.TempDir()
	svc := newPayloadService(t, out)

	reg, err := artifact.NewStubRegistry("")
	if err != nil {
		t.Fatalf("NewStubRegistry: %v", err)
	}
	stub, _, err := reg.Get("exe")
	if err != nil {
		t.Fatalf("Get(exe): %v", err)
	}

	res, err := svc.GeneratePayload("tester", &PayloadSpec{
		Transport: "tcp", Host: "1.2.3.4", Port: "4444",
	})
	if err != nil {
		t.Fatalf("GeneratePayload: %v", err)
	}
	if res.Name == "" || res.Form != "exe" || res.DownloadURL == "" {
		t.Fatalf("结果异常: %+v", res)
	}

	data, err := os.ReadFile(filepath.Join(out, res.Name, res.Name+".exe"))
	if err != nil {
		t.Fatalf("读取产物: %v", err)
	}
	if len(data) != len(stub) {
		t.Fatalf("产物长度 %d 应等于 stub %d(未编译则长度不变)", len(data), len(stub))
	}
	// 差异必须全部落在 16KB 配置块内
	first, last := -1, -1
	for i := range stub {
		if stub[i] != data[i] {
			if first < 0 {
				first = i
			}
			last = i
		}
	}
	if first < 0 {
		t.Fatal("产物应与 stub 在配置块区域存在差异(否则未烧录)")
	}
	if span := last - first + 1; span > 16384 {
		t.Fatalf("字节差异跨度 %d 超过 16KB 配置块(说明发生了编译而非纯烧录)", span)
	}

	// spec.json 含本次 dd_key(服务器侧 0600)
	specJS, err := os.ReadFile(filepath.Join(out, res.Name, "spec.json"))
	if err != nil {
		t.Fatalf("读取 spec.json: %v", err)
	}
	var m map[string]any
	if err := json.Unmarshal(specJS, &m); err != nil {
		t.Fatalf("spec.json 解析: %v", err)
	}
	if dd, _ := m["dd_key"].(string); len(dd) != 64 {
		t.Errorf("spec.json 应含 64hex dd_key, got %q", dd)
	}
	// run.cmd 存在
	if _, err := os.Stat(filepath.Join(out, res.Name, "run.cmd")); err != nil {
		t.Errorf("run.cmd 应存在: %v", err)
	}
}

// TestGeneratePayloadInvalid 4xx 错误分类: 缺字段 → ErrInvalid; 未知形态 → ErrNotFound。
func TestGeneratePayloadInvalid(t *testing.T) {
	svc := newPayloadService(t, t.TempDir())

	var se *Error
	if _, err := svc.GeneratePayload("tester", &PayloadSpec{Transport: "tcp", Host: "1.2.3.4"}); !errors.As(err, &se) || se.Class != ErrInvalid {
		t.Fatalf("缺 port 应 ErrInvalid, got %v", err)
	}
	if _, err := svc.GeneratePayload("tester", &PayloadSpec{Transport: "pipe"}); !errors.As(err, &se) || se.Class != ErrInvalid {
		t.Fatalf("未知 transport 应 ErrInvalid, got %v", err)
	}
	// 形态不支持的 stub → ErrNotFound
	if _, err := svc.GeneratePayload("tester", &PayloadSpec{Transport: "tcp", Host: "h", Port: "1", Form: "dll"}); !errors.As(err, &se) || se.Class != ErrNotFound {
		t.Fatalf("未知形态应 ErrNotFound, got %v", err)
	}
}

// TestPayloadFileTraversal 防目录穿越: basename 白名单外的名字一律拒绝。
func TestPayloadFileTraversal(t *testing.T) {
	svc := newPayloadService(t, t.TempDir())
	for _, bad := range []string{"../../etc/passwd", "..", ".", "a/b", `a\b`, ""} {
		if _, _, err := svc.PayloadFile(bad); err == nil {
			t.Errorf("非法产物名 %q 应被拒", bad)
		}
	}
	// 合法名但不存在 → 仍未找到(不落到穿越)
	if _, _, err := svc.PayloadFile("nope"); err == nil {
		t.Errorf("不存在的合法名应返回错误")
	}
}

// 落盘文件完整 —— 若退化为直写, 并发下必然出现半截文件, 长度断言即失败。
func TestGeneratePayloadConcurrentSameName(t *testing.T) {
	out := t.TempDir()
	svc := newPayloadService(t, out)

	reg, err := artifact.NewStubRegistry("")
	if err != nil {
		t.Fatalf("NewStubRegistry: %v", err)
	}
	stub, _, err := reg.Get("exe")
	if err != nil {
		t.Fatalf("Get(exe): %v", err)
	}

	const n = 8
	var wg sync.WaitGroup
	errs := make([]error, n)
	for i := 0; i < n; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			_, errs[i] = svc.GeneratePayload("", &PayloadSpec{
				Transport: "tcp", Host: "1.2.3.4", Port: "4444", Name: "same",
			})
		}(i)
	}
	wg.Wait()
	for i, e := range errs {
		if e != nil {
			t.Fatalf("goroutine %d 失败: %v", i, e)
		}
	}
	data, err := os.ReadFile(filepath.Join(out, "same", "same.exe"))
	if err != nil {
		t.Fatalf("读取产物: %v", err)
	}
	if len(data) != len(stub) {
		t.Fatalf("并发后产物长度 %d 应等于 stub %d(半截写会破坏此断言)", len(data), len(stub))
	}
	// 临时文件不残留(D.14: CreateTemp 实际模式为 same.exe.tmp-*, 用 Glob 匹配而非精确名)
	tmps, gerr := filepath.Glob(filepath.Join(out, "same", "same.exe.tmp*"))
	if gerr != nil {
		t.Fatalf("Glob: %v", gerr)
	}
	if len(tmps) != 0 {
		t.Errorf("并发后应无 .tmp 残留, 实际: %v", tmps)
	}
}
