// GET 身份块(节拍合一实施计划 / S1):
// Poll 通道升级为取件端点后, GET 需要自带身份才能路由到会话并取走 outbox 帧。
//
// 落线格式:
//
//	GET <beacon_url>?m=<base32url_nopadding( 密文 )>
//	密文 = 与 POST body 同一套加密:
//	  X25519 模式: [key_id 8B 大端][nonce||ct]  —— Handler.decrypt/DecryptFrame 直接可解
//	  PSK 模式:    Box.Encrypt 输出
//	  明文模式:    JSON 本身(仅开发/降级, 与 POST 明文模式对等)
//	明文 JSON = {"mid":"<machine_id>"} 或 {"sid":<int>}, 另加 "ts"(unix 秒) 与 "nonce"(16B hex)。
//
// 两态身份(锁死  镜像坑: 匿名注册风暴的镜像 —— 身份块缺失 → 认领失败 → 匿名会话):
//
//	注册前(sid=0):  带 mid, 服务端按 machine_id 认领既有记录, **不得新建**;
//	注册后(sid>0):  带 sid, 按 sid 路由到既有会话(含服务端重启后的持久复活)。
//
// 校验语义(S1.2): 缺 ts / 超窗 / nonce 重放 / JSON 坏 / 解密失败 → 400,
// **不可静默降级为"无命令"**(身份坏与无命令必须可区分, 否则故障被掩盖);
// 无 m= 查询参数 → 探活护栏, 保持 200 空(旧 Agent / 裸探活不受影响)。
package beacon

import (
	"encoding/base32"
	"encoding/json"
	"fmt"
	"sync"
	"time"
)

// b32url 身份块编码: 小写 RFC4648 URL-safe 无填充(查询参数友好, 无 +/= 转义问题)。
var b32url = base32.NewEncoding("abcdefghijklmnopqrstuvwxyz234567").WithPadding(base32.NoPadding)

// identityReplayWindow 身份块时间窗(秒), 与会话帧 checkReplay 的 replayWindow 同值对称:
// GET 最低节拍为注册期 1s 快速档, 120s 窗口足够容纳节拍上限(60s)与抖动。
const identityReplayWindow = 120

// identityNonceLen 身份块 nonce 的 hex 长度(16 字节)。
const identityNonceLen = 32

// IdentityBlock 身份块明文结构。
type IdentityBlock struct {
	MachineID string `json:"mid,omitempty"` // 注册前态: machine_id 认领
	SID       int    `json:"sid,omitempty"` // 注册后态: session_id 路由
	TS        int64  `json:"ts"`            // unix 秒
	Nonce     string `json:"nonce"`         // 16B hex, 防重放
}

// ParsedIdentity 解析结果; KeyID 供响应加密(X25519 模式响应与请求同 key_id)。
type ParsedIdentity struct {
	MachineID string
	SID       int
	KeyID     uint64
}

// parseIdentityBlock 从查询参数解出并校验身份块。任何一步失败都返回 error(→ 400)。
func (h *Handler) parseIdentityBlock(mParam string) (ParsedIdentity, error) {
	raw, err := b32url.DecodeString(mParam)
	if err != nil {
		return ParsedIdentity{}, fmt.Errorf("base32 解码失败: %w", err)
	}
	plain, keyID, err := h.decrypt(raw)
	if err != nil {
		return ParsedIdentity{}, fmt.Errorf("解密失败: %w", err)
	}
	var blk IdentityBlock
	if err := json.Unmarshal(plain, &blk); err != nil {
		return ParsedIdentity{}, fmt.Errorf("JSON 解析失败: %w", err)
	}
	now := time.Now().Unix()
	if blk.TS <= 0 || now-blk.TS > identityReplayWindow || blk.TS-now > identityReplayWindow {
		return ParsedIdentity{}, fmt.Errorf("时间戳缺失/超窗(ts=%d)", blk.TS)
	}
	if len(blk.Nonce) != identityNonceLen {
		return ParsedIdentity{}, fmt.Errorf("nonce 缺失/长度非法(len=%d)", len(blk.Nonce))
	}
	if !h.nonces.first(blk.Nonce, now) {
		return ParsedIdentity{}, fmt.Errorf("nonce 重放")
	}
	if blk.MachineID == "" && blk.SID <= 0 {
		return ParsedIdentity{}, fmt.Errorf("身份缺失(既无 mid 也无 sid)")
	}
	return ParsedIdentity{MachineID: blk.MachineID, SID: blk.SID, KeyID: keyID}, nil
}

// nonceGuard 并发安全的 nonce 去重。内存级(服务端重启即失效), 与会话表同生命周期;
// 防的是"同一密文被原样重放"—— 密钥持有者伪造新块不受此层约束(那是签名/密钥层的职责)。
type nonceGuard struct {
	mu   sync.Mutex
	seen map[string]int64 // nonce -> 首见时刻(unix 秒)
}

// first 报告 nonce 是否首次出现; 首见则登记。
// 惰性清理: 条目达上限时先淘汰窗外项; 若窗外清理后仍满(异常高频), 拒绝以保护内存。
func (g *nonceGuard) first(nonce string, now int64) bool {
	g.mu.Lock()
	defer g.mu.Unlock()
	if g.seen == nil {
		g.seen = make(map[string]int64)
	}
	if _, dup := g.seen[nonce]; dup {
		return false
	}
	if len(g.seen) >= 4096 {
		for k, t := range g.seen {
			if now-t > identityReplayWindow {
				delete(g.seen, k)
			}
		}
		if len(g.seen) >= 4096 {
			return false
		}
	}
	g.seen[nonce] = now
	return true
}
