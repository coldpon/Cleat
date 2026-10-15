package beacon

import (
	"bytes"
	"encoding/binary"
	"testing"
)

// TestFragmentAndReassemble 分片后按序重组, 且乱序到达也能收齐。
func TestFragmentAndReassemble(t *testing.T) {
	data := make([]byte, 500)
	for i := range data {
		data[i] = byte(i)
	}
	frags := fragment(data, 110, 7)
	if len(frags) != 5 { // 500/110 = 4.54 → 5 片
		t.Fatalf("分片数应为 5, got %d", len(frags))
	}
	for i, f := range frags {
		if seq, idx, total, ok := parseDatagramHeader(f); !ok || seq != 7 || idx != uint8(i) || total != 5 {
			t.Fatalf("片%d 头不合法: seq=%d idx=%d total=%d ok=%v", i, seq, idx, total, ok)
		}
	}

	// 逆序投递
	r := newReassembler()
	for i := len(frags) - 1; i >= 0; i-- {
		out, done := r.push(frags[i])
		if done && i != 0 {
			t.Fatalf("片%d 提前判定收齐", i)
		}
		if i == 0 {
			if !done {
				t.Fatal("乱序投递后应能收齐")
			}
			if !bytes.Equal(out, data) {
				t.Fatal("重组结果与原文不一致")
			}
		}
	}
}

// TestFragmentEmpty 空载荷不产生分片(与 Agent 侧 Fragment 的空输入行为一致)。
func TestFragmentEmpty(t *testing.T) {
	if frags := fragment(nil, 110, 1); len(frags) != 0 {
		t.Fatalf("空载荷应产生 0 片, got %d", len(frags))
	}
}

// TestFragmentTooManyFrags 片数超 255 时返回 nil(协议头 total 仅 1B, 显式失败不截断)。
func TestFragmentTooManyFrags(t *testing.T) {
	data := make([]byte, 256) // 每片 1B → 256 片 > 255
	if frags := fragment(data, 1, 1); frags != nil {
		t.Fatalf("超 255 片应返回 nil, got %d 片", len(frags))
	}
	// 恰好 255 片应通过
	if frags := fragment(make([]byte, 255), 1, 1); len(frags) != 255 {
		t.Fatalf("255 片应通过, got %d", len(frags))
	}
}

// TestReassemblerDuplicateFrag 重复片不计入 received, 不放大字节数。
func TestReassemblerDuplicateFrag(t *testing.T) {
	r := newReassembler()
	frags := fragment([]byte("hello world"), 4, 3) // 3 片
	for i := 0; i < 5; i++ {
		if _, done := r.push(frags[0]); done {
			t.Fatalf("仅重复第 1 片不应收齐(第 %d 次)", i)
		}
	}
	if _, done := r.push(frags[1]); done {
		t.Fatal("缺第 3 片不应收齐")
	}
	if _, ok := r.push(frags[0]); ok {
		t.Fatal("重复片不应触发收齐")
	}
	out, done := r.push(frags[2])
	if !done || string(out) != "hello world" {
		t.Fatalf("收齐后应为 hello world, got %q done=%v", out, done)
	}
}

// TestReassemblerRejectsBadHeader 非法分片被丢弃(魔数/片长/片序/总片数)。
func TestReassemblerRejectsBadHeader(t *testing.T) {
	r := newReassembler()
	good := fragment([]byte("abcd"), 4, 1)[0]
	bads := map[string][]byte{
		"魔数错":   append([]byte{0x00}, good[1:]...),
		"太短":    good[:3],
		"片长不符":  mutFrag(good, func(c []byte) { binary.LittleEndian.PutUint16(c[5:7], 99) }),
		"总片数为0": mutFrag(good, func(c []byte) { c[4] = 0 }),
		"片序越界":  mutFrag(good, func(c []byte) { c[3] = 1; c[4] = 1 }),
	}
	for name, bad := range bads {
		if _, done := r.push(bad); done {
			t.Errorf("%s: 不应视为完成", name)
		}
	}
}

// TestReassemblerSameSeqDiffTotal 同报序号但总片数不一致时丢弃旧报文重新开始。
func TestReassemblerSameSeqDiffTotal(t *testing.T) {
	r := newReassembler()
	a := fragment([]byte("abcdef"), 3, 9) // 2 片
	if _, done := r.push(a[0]); done {    // 交上 1 片
		t.Fatal("不完整不应收齐")
	}
	b := fragment([]byte("xyz"), 3, 9) // 同 seq, 1 片
	out, done := r.push(b[0])          // 总片数不一致 → 重置后本片即收齐
	if !done || string(out) != "xyz" {
		t.Fatalf("同 seq 异 total 应重置并收齐新报文, got %q done=%v", out, done)
	}
}

// TestReassemblerTimeout 超 30s 未收齐的报文中途作废, 续片无法凑成原报文。
func TestReassemblerTimeout(t *testing.T) {
	r := newReassembler()
	now := int64(1000)
	r.nowMs = func() int64 { return now }

	frags := fragment([]byte("abcdef"), 3, 5) // 2 片
	if _, done := r.push(frags[0]); done {
		t.Fatal("不完整不应收齐")
	}
	now += reassemblyTimeoutMs + 1 // 推过超时窗口
	if _, done := r.push(frags[1]); done {
		t.Fatal("过期条目应被回收, 续片不应凑成报文")
	}
}

// TestReassemblerEntryLimit 条目上限只拒新报序号, 在途报文续片不受限。
func TestReassemblerEntryLimit(t *testing.T) {
	r := newReassembler()
	// 灌满 maxPendingEntries 个不完整报文(每片小, 不触发字节上限)
	for i := 0; i < maxPendingEntries; i++ {
		frags := fragment([]byte("payload"), 4, uint16(i)) // 2 片
		if _, done := r.push(frags[0]); done {
			t.Fatalf("seq=%d 不完整不应收齐", i)
		}
	}
	// 新 seq 被拒
	extra := fragment([]byte("payload"), 4, 999)
	if _, done := r.push(extra[0]); done {
		t.Fatal("表满时新 seq 应被拒")
	}
	// 在途 seq 的续片仍被接受(否则在途报文永远收不齐)
	if _, done := r.push(extra[0]); done {
		t.Fatal("被拒的新 seq 不应入表")
	}
	first := fragment([]byte("payload"), 4, 0)
	if out, done := r.push(first[1]); !done || string(out) != "payload" {
		t.Fatalf("在途报文续片应被接受并收齐, got %q done=%v", out, done)
	}
}

// mutFrag 复制分片并应用改写(避免测试间共享底层数组)。
func mutFrag(frag []byte, f func([]byte)) []byte {
	c := append([]byte(nil), frag...)
	f(c)
	return c
}
