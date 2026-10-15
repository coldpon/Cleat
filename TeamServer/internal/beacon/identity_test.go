package beacon

import (
	"encoding/json"
	"fmt"
	"strings"
	"testing"
	"time"

	"cleat/teamserver/internal/crypto"
	"cleat/teamserver/internal/eventbus"
	"cleat/teamserver/internal/session"
	"cleat/teamserver/internal/store"
)

// newTestHandler 构造明文模式(无加密器)的 Handler, 供身份块格式/校验测试。
func newTestHandler(t *testing.T) *Handler {
	t.Helper()
	mgr := session.NewManager(store.New(), eventbus.New(), nil, nil, false)
	return New(mgr, nil, nil)
}

// buildBlock 构造身份块明文 JSON 并 base32 编码(明文模式: 密文 = JSON 本身)。
func buildBlock(t *testing.T, mid string, sid int, ts int64, nonce string) string {
	t.Helper()
	blk := IdentityBlock{MachineID: mid, SID: sid, TS: ts, Nonce: nonce}
	raw, err := json.Marshal(blk)
	if err != nil {
		t.Fatalf("marshal block: %v", err)
	}
	return b32url.EncodeToString(raw)
}

func validNonce() string { return strings.Repeat("ab", 16) } // 16B hex

// TestParseIdentityBlockOK 锁定两态身份的合法路径:
// mid 态(注册前)与 sid 态(注册后)都应解出对应身份; KeyID 在明文模式恒为 0。
func TestParseIdentityBlockOK(t *testing.T) {
	h := newTestHandler(t)
	now := time.Now().Unix()

	got, err := h.parseIdentityBlock(buildBlock(t, "machine-abc", 0, now, strings.Repeat("aa", 16)))
	if err != nil {
		t.Fatalf("mid 态应解析成功: %v", err)
	}
	if got.MachineID != "machine-abc" || got.SID != 0 {
		t.Fatalf("mid 态身份不符: %+v", got)
	}

	// 每块必须用不同 nonce(真实 Agent 每次随机); 同 nonce 会被守卫拒绝
	got2, err := h.parseIdentityBlock(buildBlock(t, "", 42, now, strings.Repeat("bb", 16)))
	if err != nil {
		t.Fatalf("sid 态应解析成功: %v", err)
	}
	if got2.SID != 42 || got2.MachineID != "" {
		t.Fatalf("sid 态身份不符: %+v", got2)
	}
}

// TestParseIdentityBlockPSK PSK 模式: 密文 = Box.Encrypt(JSON), 解密后身份一致。
func TestParseIdentityBlockPSK(t *testing.T) {
	psk := strings.Repeat("11", 32) // 32B hex = 16B 密钥
	cbox, err := crypto.New(psk)
	if err != nil {
		t.Fatalf("New(psk): %v", err)
	}
	mgr := session.NewManager(store.New(), eventbus.New(), cbox, nil, false)
	h := New(mgr, cbox, nil)

	plain, _ := json.Marshal(IdentityBlock{MachineID: "m-psk", TS: time.Now().Unix(), Nonce: validNonce()})
	ct, err := cbox.Encrypt(plain)
	if err != nil {
		t.Fatalf("Encrypt: %v", err)
	}
	got, err := h.parseIdentityBlock(b32url.EncodeToString(ct))
	if err != nil {
		t.Fatalf("PSK 模式应解析成功: %v", err)
	}
	if got.MachineID != "m-psk" {
		t.Fatalf("PSK 身份不符: %+v", got)
	}
}

// TestParseIdentityBlockReject 拒绝路径全集(S1.3):
// 坏 base32 / 坏 JSON / 缺 ts / 超窗 / nonce 长度非法 / nonce 重放 / 两态身份全缺。
// 每条对应一次 400 —— 身份坏不可静默降级为"无命令"。
func TestParseIdentityBlockReject(t *testing.T) {
	h := newTestHandler(t)
	now := time.Now().Unix()

	cases := []struct {
		name  string
		param string
	}{
		{"坏base32", "not!!valid@@base32"},
		{"base32但坏JSON", b32url.EncodeToString([]byte("no-json-here"))},
		{"缺ts", buildBlock(t, "m1", 0, 0, validNonce())},
		{"超窗-过去", buildBlock(t, "m1", 0, now-identityReplayWindow-10, validNonce())},
		{"超窗-未来", buildBlock(t, "m1", 0, now+identityReplayWindow+10, validNonce())},
		{"nonce太短", buildBlock(t, "m1", 0, now, "abcd")},
		{"两态身份全缺", buildBlock(t, "", 0, now, validNonce())},
	}
	for _, c := range cases {
		if _, err := h.parseIdentityBlock(c.param); err == nil {
			t.Fatalf("%s 应被拒绝", c.name)
		}
	}

	// nonce 重放: 同一参数第二次解析必须失败(前两次用不同 nonce 的合法块占位验证守卫活着)
	p1 := buildBlock(t, "m1", 0, now, strings.Repeat("11", 16))
	if _, err := h.parseIdentityBlock(p1); err != nil {
		t.Fatalf("首次 nonce 应通过: %v", err)
	}
	if _, err := h.parseIdentityBlock(p1); err == nil {
		t.Fatalf("同 nonce 重放应被拒绝")
	}
}

// TestNonceGuardBounded 守卫的内存有界性:
// 窗内满载时拒绝新 nonce(保护上限); 时间推进超窗后惰性清理, 新 nonce 可再次登记。
// (重放防护语义由 ts 窗口前置承担 —— 窗外块到不了 nonce 校验, 守卫只负责内存不膨胀。)
func TestNonceGuardBounded(t *testing.T) {
	g := &nonceGuard{}
	now := time.Now().Unix()

	// 填满守卫上限(4096)
	for i := 0; i < 4096; i++ {
		nonce := fmt.Sprintf("%08x", i) + strings.Repeat("0", 16) // 唯一 32 hex
		if !g.first(nonce, now) {
			t.Fatalf("第 %d 个窗内 nonce 不应被拒", i)
		}
	}
	// 全部窗内: 清理不掉 → 新 nonce 被拒(有界保护)
	if g.first(strings.Repeat("ff", 16), now) {
		t.Fatal("窗内满载时新 nonce 应被拒(保护上限)")
	}
	// 时间推进超窗: 清理后新 nonce 可登记
	later := now + identityReplayWindow + 1
	if !g.first(strings.Repeat("ee", 16), later) {
		t.Fatal("超窗清理后新 nonce 应可登记")
	}
}
