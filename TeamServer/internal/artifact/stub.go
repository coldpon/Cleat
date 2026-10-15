//
// 背景: 模板化烧录(patch.go)要求一个"无配置 Release 构建的 Agent 可执行体",
// 内含 16KB 配置块(cfgMagic)。该 stub 随本包发布(go:embed 兜底), 运行期亦可用
// -stub-dir 覆盖为外部目录(便于换构建/stub 落后时热替换)。
//
// 设计:
//   - embed 兜底: `//go:embed stubs` 把 stubs/ 目录(含 stub.json 清单 + 各形态二进制)编进二进制,
//     交付机无需携带任何构建产物;
//   - 外部覆盖: NewStubRegistry(overrideDir) 中, overrideDir 非空且存在同名文件时优先外部;
//   - 启动即校验: 每个 stub 必须含 cfgMagic(否则不是可烧录模板), 不合格则跳过不注册(告警)。
package artifact

import (
	"bytes"
	"crypto/sha256"
	"embed"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

// stubFS 内嵌 stubs/ 目录(清单 + 各形态二进制)。目录须在编译期存在且非空。
//
//go:embed stubs
var stubFS embed.FS

// ErrStubNotFound 请求的形态无可用 stub。
var ErrStubNotFound = errors.New("stub 不存在")

// stubManifestName 内置清单文件名(相对 embed 根)。
const stubManifestName = "stubs/stub.json"

// StubInfo stub 元数据(随 stubs/stub.json 发布)。
// size/sha256 在加载时按实际字节重算(文件是权威来源), 清单中的值仅作参考。
type StubInfo struct {
	Form         string `json:"form"`          // exe|dll|shc（P0: exe）
	File         string `json:"file"`          // stubs/cleatagent-stub.exe（相对 embed 根）
	AgentCommit  string `json:"agent_commit"`  // 产出该 stub 的 Agent 提交
	BlockVersion int    `json:"block_version"` // = cfgVersion(=1)
	Size         int64  `json:"size"`
	SHA256       string `json:"sha256"`
	BuiltAt      string `json:"built_at"`
}

// StubRegistry stub 注册表(embed 兜底 + 外部目录覆盖)。
// 启动时一次性加载(元数据小、二进制按需即可, 但为简化实现这里也随启动加载)。
type StubRegistry struct {
	overrideDir string              // 空=仅 embed
	metas       map[string]StubInfo // form -> 元数据
	blobs       map[string][]byte   // form -> 字节
}

// NewStubRegistry 构造注册表。overrideDir 非空且存在对应文件时优先外部; 否则用 embed。
// 对每个 stub 校验 cfgMagic 存在(patch.go: cfgMagic), 失败则不注册并跳过。
func NewStubRegistry(overrideDir string) (*StubRegistry, error) {
	r := &StubRegistry{
		overrideDir: overrideDir,
		metas:       map[string]StubInfo{},
		blobs:       map[string][]byte{},
	}
	manifest, err := stubFS.ReadFile(stubManifestName)
	if err != nil {
		return nil, fmt.Errorf("读取内置 stub 清单失败: %w", err)
	}
	var infos []StubInfo
	if err := json.Unmarshal(manifest, &infos); err != nil {
		return nil, fmt.Errorf("解析内置 stub 清单失败: %w", err)
	}
	for _, info := range infos {
		form := strings.TrimSpace(info.Form)
		if form == "" {
			continue
		}
		blob, err := r.loadBytes(info)
		if err != nil {
			continue // 该形态不可用: 跳过(不注册), 不影响其它形态
		}
		if !bytes.Contains(blob, cfgMagic) {
			continue // 不含配置块魔数 → 非可烧录模板, 跳过
		}
		info.Form = form
		info.Size = int64(len(blob))
		sum := sha256.Sum256(blob)
		info.SHA256 = hex.EncodeToString(sum[:])
		r.metas[form] = info
		r.blobs[form] = blob
	}
	return r, nil
}

// loadBytes 取某形态 stub 字节: 外部覆盖优先, 否则 embed。
func (r *StubRegistry) loadBytes(info StubInfo) ([]byte, error) {
	if r.overrideDir != "" {
		p := filepath.Join(r.overrideDir, filepath.Base(info.File))
		if b, err := os.ReadFile(p); err == nil {
			return b, nil
		}
	}
	return stubFS.ReadFile(info.File)
}

// List 返回已注册 stub 元数据(按 form 升序, 稳定输出)。
func (r *StubRegistry) List() []StubInfo {
	out := make([]StubInfo, 0, len(r.metas))
	for _, m := range r.metas {
		out = append(out, m)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Form < out[j].Form })
	return out
}

// Get 返回某形态 stub 字节 + 元数据; 不存在返回 (nil, StubInfo{}, ErrStubNotFound)。
// form 为空按 exe 处理(P0 默认形态)。
func (r *StubRegistry) Get(form string) ([]byte, StubInfo, error) {
	if form == "" {
		form = "exe"
	}
	blob, ok := r.blobs[form]
	if !ok {
		return nil, StubInfo{}, ErrStubNotFound
	}
	return blob, r.metas[form], nil
}
