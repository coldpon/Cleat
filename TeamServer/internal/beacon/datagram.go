// 数据报分片/重组(DNS/ICMP 通道共用) —— Go 侧实现, 与 Agent 侧
// `Core/Comms/Transports/Internal/DatagramCodec.hpp/.cpp` 锁步。
//
// 分片头(7B): [1B magic=0xA7][2B 报序号 小端][1B 片序号][1B 总片数][2B 片长 小端] + 片数据
//
// 语义对齐点:
//   - 乱序容忍: 按片序号聚合, 收齐总片数才拼接;
//   - 重复片不计入 received/bytes(防重复片放大);
//   - 同报序号但总片数不一致 = 协议错乱, 原地重置该条目;
//   - 30s 未更新的条目回收;
//   - 条目数上限只约束**新**报序号(在途报文不受限, 否则表满时续片被拒 → 永远收不齐),
//     另加在途总字节上限(单片负载可达 64KB, 条目上限不封字节);
//   - 分片数 > 255 时 Fragment 返回 nil(协议头 total 仅 1B; 显式失败而非截断)。
package beacon

import (
	"encoding/binary"
	"sync"
	"time"
)

const (
	datagramMagic      = 0xA7
	datagramHeaderSize = 7

	// 与 Agent 侧 DatagramCodec.hpp 的同名常量取值一致
	reassemblyTimeoutMs = 30000
	maxPendingEntries   = 128
	maxPendingBytes     = 4 * 1024 * 1024
)

// fragment 把 data 切成数据报分片(每片 = 7B 头 + ≤maxPayload 字节负载)。
// seq 为本次报文的报序号, 由调用方持有并发号(Agent 侧用进程内原子计数器递增)。
// 返回 nil 表示片数超 255 上限(协议头 total 只有 1B)。
func fragment(data []byte, maxPayload int, seq uint16) [][]byte {
	if maxPayload <= 0 {
		maxPayload = 1
	}
	total := (len(data) + maxPayload - 1) / maxPayload
	if total > 255 {
		return nil
	}
	out := make([][]byte, 0, total)
	for i := 0; i < total; i++ {
		off := i * maxPayload
		n := len(data) - off
		if n > maxPayload {
			n = maxPayload
		}
		chunk := make([]byte, datagramHeaderSize+n)
		chunk[0] = datagramMagic
		binary.LittleEndian.PutUint16(chunk[1:3], seq)
		chunk[3] = byte(i)
		chunk[4] = byte(total)
		binary.LittleEndian.PutUint16(chunk[5:7], uint16(n))
		copy(chunk[datagramHeaderSize:], data[off:off+n])
		out = append(out, chunk)
	}
	return out
}

// parseDatagramHeader 解析分片头, 校验与 Agent 侧 ParseHeader 逐条一致:
// 魔数 + 总片数非零 + 片序号小于总片数 + 片长等于实际负载长度。
func parseDatagramHeader(chunk []byte) (seq uint16, idx, total uint8, ok bool) {
	if len(chunk) < datagramHeaderSize || chunk[0] != datagramMagic {
		return 0, 0, 0, false
	}
	seq = binary.LittleEndian.Uint16(chunk[1:3])
	idx = chunk[3]
	total = chunk[4]
	fragLen := binary.LittleEndian.Uint16(chunk[5:7])
	if total == 0 || idx >= total || int(fragLen) != len(chunk)-datagramHeaderSize {
		return 0, 0, 0, false
	}
	return seq, idx, total, true
}

// pendingDatagram 单个报文的在途重组状态。
type pendingDatagram struct {
	total      uint8
	received   int
	bytes      int
	parts      map[uint8][]byte
	lastSeenMs int64
}

// reassembler 按报序号聚合分片的接收侧重组器。零值不可用, 用 newReassembler 构造。
// 每个对端(见 dnsPeer)独占一个实例 —— 报序号是各 Agent 进程内独立递增的, 跨对端会串号。
type reassembler struct {
	mu      sync.Mutex
	entries map[uint16]*pendingDatagram
	nowMs   func() int64 // 单测可注入
}

func newReassembler() *reassembler {
	return &reassembler{
		entries: make(map[uint16]*pendingDatagram),
		nowMs:   func() int64 { return time.Now().UnixMilli() },
	}
}

// push 投递一个分片(含 7B 头)。收齐一整个报文时返回 (完整报文, true)。
func (r *reassembler) push(chunk []byte) ([]byte, bool) {
	seq, idx, total, ok := parseDatagramHeader(chunk)
	if !ok {
		return nil, false // 非法分片: 丢弃(不视为完成)
	}
	payload := chunk[datagramHeaderSize:]
	now := r.nowMs()

	r.mu.Lock()
	defer r.mu.Unlock()

	// 清理超时条目并累计在途字节(单遍扫描)
	pendingBytes := 0
	for k, v := range r.entries {
		if now-v.lastSeenMs > reassemblyTimeoutMs {
			delete(r.entries, k)
			continue
		}
		pendingBytes += v.bytes
	}

	// 条目上限只拒新报序号: 否则表满时连续片都被拒, 在途报文永远收不齐
	p := r.entries[seq]
	if p == nil && len(r.entries) >= maxPendingEntries {
		return nil, false
	}
	if pendingBytes+len(payload) > maxPendingBytes {
		return nil, false
	}
	if p == nil {
		p = &pendingDatagram{parts: make(map[uint8][]byte)}
		r.entries[seq] = p
	}
	if p.total != 0 && p.total != total {
		// 同报序号异总片数: 协议错乱, 丢弃旧报文重新开始(原地重置, 不换 map 条目)
		*p = pendingDatagram{parts: make(map[uint8][]byte)}
	}
	p.total = total
	p.lastSeenMs = now

	// 重复片不计入计数与字节数
	if _, dup := p.parts[idx]; !dup {
		p.parts[idx] = append([]byte(nil), payload...)
		p.received++
		p.bytes += len(payload)
	}

	if p.received != int(p.total) {
		return nil, false
	}
	// 按片序号升序拼接
	out := make([]byte, 0, p.bytes)
	for i := uint8(0); i < p.total; i++ {
		out = append(out, p.parts[i]...)
	}
	delete(r.entries, seq)
	return out, true
}
