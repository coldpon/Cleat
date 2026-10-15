// 服务端实现 —— 模板化配置写入(CS 式 artifact 机制一期, 零编译)
//
// 用法: 预编译的 Agent 模板(exe/dll, 任意构建产物)内含 16KB 配置块,
// 操作员配置 → 本工具把配置写入块内 → 秒级出样本, 无需重新编译。
//
// 块布局(与 Agent Core/Config/EmbeddedConfig.hpp 锁步, 两处注释互指):
//
//	[0..15]   魔数(伪随机 16B, 非字符串)
//	[16]      版本(1)
//	[17]      状态(0=模板空 1=已配置)
//	[18..19]  载荷长度(大端 uint16)
//	[20..]    XOR(0x5A) 后的 JSON(≤16364 字节)
//	其余      0x00 填充(固定 16384B → patch 不改文件长度, PE 结构零改动)
package artifact

import (
	"bytes"
	"encoding/json"
	"fmt"
	"hash/fnv"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
)

// 与 Agent kCfgMagic / kCfgBlobSize / kCfgVersion / kCfgStateSet / kCfgXorKey 一致
var cfgMagic = []byte{
	0x8B, 0xE4, 0x1F, 0x73, 0xC2, 0x5A, 0x90, 0x3D,
	0x47, 0xB1, 0x6E, 0x0A, 0xD9, 0x24, 0xEC, 0x51,
}

const (
	cfgBlobSize = 16384
	cfgVersion  = 1
	cfgStateSet = 1
	cfgXorKey   = 0x5A
	cfgHeadLen  = 20 // 魔数16 + 版本1 + 状态1 + 长度2
)

// PatchTemplate 把配置写入模板二进制的配置块并输出新样本(读文件→patch→原子写)。
// 保留原始签名以兼容既有调用点(service/artifact.go:36)。
// cfg 为 Agent 认识的配置字段(transport/host/port/url/upload_url/dns_server/
// dns_domain/icmp_target/user_agent/psk/server_pubkey, 以及可调参数
// heartbeat/poll 等 AgentConfig::ApplyJson 支持的域)。
func PatchTemplate(tmplPath, outPath string, cfg map[string]any) error {
	d, err := os.ReadFile(tmplPath)
	if err != nil {
		return fmt.Errorf("读取模板失败: %w", err)
	}
	out, err := PatchBytes(d, cfg)
	if err != nil {
		return err
	}
	return patchAtomic(outPath, out)
}

// PatchBytes 内存版: 在模板字节上写入配置块, 返回烧录后的**新副本**(不改入参)。
// 校验: magic 存在 + 载荷 ≤ cfgBlobSize-cfgHeadLen(16364)。
func PatchBytes(tmpl []byte, cfg map[string]any) ([]byte, error) {
	off := bytes.Index(tmpl, cfgMagic)
	if off < 0 {
		return nil, fmt.Errorf("模板中未找到配置块魔数(非 CleatAgent 构建产物?)")
	}
	if off+cfgBlobSize > len(tmpl) {
		return nil, fmt.Errorf("配置块越界: off=%d blob=%d file=%d", off, cfgBlobSize, len(tmpl))
	}

	payload, err := json.Marshal(cfg)
	if err != nil {
		return nil, fmt.Errorf("配置序列化失败: %w", err)
	}
	if len(payload) > cfgBlobSize-cfgHeadLen {
		return nil, fmt.Errorf("配置过大: %d 字节 > 上限 %d(16KB 块减头部)", len(payload), cfgBlobSize-cfgHeadLen)
	}

	blob := make([]byte, cfgBlobSize)
	copy(blob, cfgMagic)
	blob[16] = cfgVersion
	blob[17] = cfgStateSet
	blob[18] = byte(len(payload) >> 8)
	blob[19] = byte(len(payload))
	for i, b := range payload {
		blob[cfgHeadLen+i] = b ^ cfgXorKey
	}
	// 其余 0x00 填充

	out := make([]byte, len(tmpl))
	copy(out, tmpl)
	copy(out[off:off+cfgBlobSize], blob)
	return out, nil
}

// DecodeConfigBlock 解回样本配置块内的 JSON(自检/测试用)。
// 与 PatchBytes 互逆: 定位魔数 → 读头 → XOR 还原 → 解析 JSON。
func DecodeConfigBlock(data []byte) (map[string]any, error) {
	off := bytes.Index(data, cfgMagic)
	if off < 0 {
		return nil, fmt.Errorf("未找到配置块魔数")
	}
	if off+cfgBlobSize > len(data) {
		return nil, fmt.Errorf("配置块越界")
	}
	if data[off+16] != cfgVersion {
		return nil, fmt.Errorf("块版本不识别: %d", data[off+16])
	}
	if data[off+17] != cfgStateSet {
		return nil, fmt.Errorf("块未配置(state=%d)", data[off+17])
	}
	n := int(data[off+18])<<8 | int(data[off+19])
	if n > cfgBlobSize-cfgHeadLen {
		return nil, fmt.Errorf("载荷长度非法: %d", n)
	}
	buf := make([]byte, n)
	for i := 0; i < n; i++ {
		buf[i] = data[off+cfgHeadLen+i] ^ cfgXorKey
	}
	var out map[string]any
	if err := json.Unmarshal(buf, &out); err != nil {
		return nil, fmt.Errorf("配置 JSON 解析失败: %w", err)
	}
	return out, nil
}

// writeStripeLocks 串行化"同一目标路径"的并发原子写。
// 原因(实测并发同名生成暴露): Windows 上多线程同时把文件 rename 覆盖到同一 dst 时,
// 目标会瞬时处于 pending-delete, 另一线程的 MoveFileEx 可能失败(ACCESS_DENIED)。
// 按目标路径分片加锁即可消除该竞态; 分片(而非逐路径 map)避免 map 无界增长。
const writeStripeCount = 64

var writeStripeLocks [writeStripeCount]sync.Mutex

func writeLockFor(dstPath string) *sync.Mutex {
	h := fnv.New32a()
	_, _ = h.Write([]byte(filepath.Clean(dstPath)))
	return &writeStripeLocks[h.Sum32()%writeStripeCount]
}

// WriteFileAtomic 原子写文件(tmp → rename, 同盘原子), perm 指定权限。
// 供 service 归档 bin / spec.json(0600) / run.cmd 复用。
// 临时名唯一(os.CreateTemp): 并发写同一目标时各写各的 tmp, rename 原子且互为"最后者胜" ——
// 避免固定 "<dst>.tmp" 撞车; 再对同一 dst 分片加锁, 规避 Windows rename 覆盖竞态。
func WriteFileAtomic(dstPath string, data []byte, perm os.FileMode) error {
	mu := writeLockFor(dstPath)
	mu.Lock()
	defer mu.Unlock()

	dir := filepath.Dir(dstPath)
	tmp, err := os.CreateTemp(dir, filepath.Base(dstPath)+".tmp-*")
	if err != nil {
		return fmt.Errorf("创建临时文件失败: %w", err)
	}
	tmpName := tmp.Name()
	if _, err := tmp.Write(data); err != nil {
		_ = tmp.Close()
		_ = os.Remove(tmpName)
		return fmt.Errorf("写入临时文件失败: %w", err)
	}
	if err := tmp.Close(); err != nil {
		_ = os.Remove(tmpName)
		return fmt.Errorf("关闭临时文件失败: %w", err)
	}
	if err := os.Chmod(tmpName, perm); err != nil {
		_ = os.Remove(tmpName)
		return fmt.Errorf("设置权限失败: %w", err)
	}
	if err := os.Rename(tmpName, dstPath); err != nil {
		_ = os.Remove(tmpName)
		return fmt.Errorf("原子替换失败: %w", err)
	}
	return nil
}

// patchAtomic 把字节原子写入目标路径(tmp → rename, 同盘原子, 可执行权限 0755)。
func patchAtomic(dstPath string, blob []byte) error {
	return WriteFileAtomic(dstPath, blob, 0o755)
}

// ValidatePatchCfg A 层(启动引导)条件必填校验, 按 transport 分派 ——
// 语义对齐 Generator.Spec.Args() 的传输段(artifact.go:270-318)。
// 仅校验 A 层传输字段; 密钥(B 层)由调用方在 resolve 之后单独判定
// (生成流程: ValidatePatchCfg → 注入 server_pubkey → 组块, 见 service.GeneratePayload)。
func ValidatePatchCfg(cfg map[string]any) error {
	transport := cfgStr(cfg, "transport")
	switch transport {
	case "tcp":
		if cfgStr(cfg, "host") == "" || cfgStr(cfg, "port") == "" {
			return fmt.Errorf("tcp 模式需要 host 与 port")
		}
		// D.12: 端口范围校验(前端拦了 UI 路径, API 直发也要拒)
		if p, err := strconv.Atoi(cfgStr(cfg, "port")); err != nil || p < 1 || p > 65535 {
			return fmt.Errorf("tcp port 需为 1..65535, got %q", cfgStr(cfg, "port"))
		}
	case "https":
		if cfgStr(cfg, "url") == "" {
			return fmt.Errorf("https 模式需要 url")
		}
	case "dns":
		if cfgStr(cfg, "dns_server") == "" || cfgStr(cfg, "dns_domain") == "" {
			return fmt.Errorf("dns 模式需要 dns_server 与 dns_domain")
		}
	case "icmp":
		if cfgStr(cfg, "icmp_target") == "" {
			return fmt.Errorf("icmp 模式需要 icmp_target")
		}
	case "wss":
		if cfgStr(cfg, "wss_url") == "" {
			return fmt.Errorf("wss 模式需要 wss_url")
		}
	case "quic":
		// 启动装配只认通用 url 槽的 quic:// 前缀(FromAgentConfig 不搬 host/port 进
		// 通道配置输入), 故此处要求 URL 形态; 否则产物启动时 quic 判据不过被静默跳过。
		u := cfgStr(cfg, "url")
		if u == "" || !strings.HasPrefix(u, "quic://") {
			return fmt.Errorf("quic 模式需要 quic:// 前缀的 url(如 quic://1.2.3.4:4433), got %q", u)
		}
	default:
		return fmt.Errorf("未知传输模式 %q", transport)
	}
	return nil
}

// cfgStr 从配置 map 取字符串值(容忍 string / float64 / json.Number 数字形态)。
func cfgStr(cfg map[string]any, key string) string {
	switch v := cfg[key].(type) {
	case string:
		return strings.TrimSpace(v)
	case float64:
		return strconv.FormatFloat(v, 'f', -1, 64)
	case int:
		return strconv.Itoa(v)
	case json.Number:
		return v.String()
	}
	return ""
}
