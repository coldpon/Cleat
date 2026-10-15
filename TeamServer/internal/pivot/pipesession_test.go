package pivot

// 会话借道 TS 半边测试(明文模式, 加密盒 nil —— B 会话落线帧即明文 JSON)。
// 覆盖: 虚拟会话创建(FirstConnect 注册) / 上行(child.Write → B runLoop) /
// 下行 relay(B 会话出帧 → A 方向 PivotData) / 父离线级联。
// 注意: 子会话的 *Session 经 mgr 会话表不可公开枚举, 测试经 store.Record
// (Get/ByMachine)与 aCli 出站帧断言。

import (
	"encoding/binary"
	"encoding/json"
	"net"
	"testing"
	"time"

	"cleat/teamserver/internal/eventbus"
	"cleat/teamserver/internal/protocol"
	"cleat/teamserver/internal/session"
	"cleat/teamserver/internal/store"
)

// waitCond 轮询条件至超时。
func waitCond(t *testing.T, d time.Duration, what string, cond func() bool) {
	t.Helper()
	deadline := time.Now().Add(d)
	for time.Now().Before(deadline) {
		if cond() {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatalf("条件在 %v 内未满足: %s", d, what)
}

// wireFrame 组装一条 B 会话落线帧([4B 大端长度][JSON 载荷])。
func wireFrame(payload []byte) []byte {
	out := make([]byte, 4+len(payload))
	binary.BigEndian.PutUint32(out[:4], uint32(len(payload)))
	copy(out[4:], payload)
	return out
}

// parseHybrid 拆混合帧(JSON 头 + '\0' + binary)。
// parseHybridTest 拆混合帧(4B 长度头 + JSON 头 + '' + binary)。
// parseHybridTest 拆混合帧(4B 长度头 + JSON 头 + 0x00 分隔 + binary)。
// JSON 起止按括号定位: 第一个 '{' 为头起点, 其后第一个 0x00 为头终点
// (长度头本身含 0x00, 不能从头扫描)。
func parseHybridTest(wire []byte) (map[string]interface{}, []byte, bool) {
	start := -1
	for k, b := range wire {
		if b == '{' {
			start = k
			break
		}
	}
	if start < 0 {
		return nil, nil, false
	}
	end := -1
	for k := start; k < len(wire); k++ {
		if wire[k] == 0 {
			end = k
			break
		}
	}
	if end < 0 {
		return nil, nil, false
	}
	var head map[string]interface{}
	if err := json.Unmarshal(wire[start:end], &head); err != nil {
		return nil, nil, false
	}
	return head, wire[end+1:], true
}

// setupPipeTest 构建 mgr/pivot/父会话(A, ID=1)与入口登记; aCli 为 A 会话
// net.Pipe 的另一端(测试从中读 A 出站帧)。
func setupPipeTest(t *testing.T) (*Pivot, *session.Session, net.Conn, *store.Store) {
	t.Helper()
	st := store.New()
	mgr := session.NewManager(st, eventbus.New(), nil, nil, false)
	if err := mgr.AddListener("127.0.0.1:0", nil, nil, false); err != nil {
		t.Fatalf("AddListener: %v", err)
	}
	piv := New(mgr, eventbus.New())

	aSrv, aCli := net.Pipe()
	aSess := session.New(protocol.NewConn(aSrv, nil, nil, false), eventbus.New())
	aSess.Record.ID = 900001 // 避开 store NewID 的低段分配(与子会话 ID 冲突会自环)
	mgr.InjectSessionForTest(aSess)

	piv.pipeEntriesMu.Lock()
	piv.pipeEntries["sp-t"] = PipeEntry{SessionID: 1, PipeName: "sp-t", Key: "k", ListenAddr: "127.0.0.1:0"}
	piv.pipeEntriesMu.Unlock()
	return piv, aSess, aCli, st
}

// registerChild 模拟 Agent B 的 FirstConnect 经 A 透传上来, 等注册完成并
// 返回 child 端(后续模拟 Agent B 上行)。

func TestPipeSessionCreateAndCascade(t *testing.T) {
	piv, aSess, _, _ := setupPipeTest(t)

	first, _ := json.Marshal(map[string]interface{}{
		"cmd":    "FirstConnect",
		"ts":     time.Now().Unix(),
		"params": map[string]interface{}{"machine_id": "machine-pipe-b"},
	})
	piv.handleSessionData(aSess, "sess-sp-t", wireFrame(first))

	piv.virtMu.Lock()
	vp := piv.virtualPipes["sess-sp-t"]
	piv.virtMu.Unlock()
	if vp == nil {
		t.Fatalf("虚拟会话应已创建")
	}

	// 未登记入口的 sess- 帧被丢弃(不创建新虚拟会话)
	piv.handleSessionData(aSess, "sess-unknown", wireFrame(first))
	if got := len(piv.VirtualLinkInfos()); got != 1 {
		t.Fatalf("链路边应有 1 条, got %d", got)
	}

	// 级联: 父会话(#1)离线 → 虚拟会话关闭
	piv.closeVirtualByParent(900001) // A 的注入 ID(与 ParentSID 一致)
	piv.virtMu.Lock()
	_, exists := piv.virtualPipes["sess-sp-t"]
	piv.virtMu.Unlock()
	if exists {
		t.Fatalf("父离线后虚拟会话应已注销")
	}
}

func TestPipeSessionUplinkAndRelay(t *testing.T) {
	piv, aSess, aCli, _ := setupPipeTest(t)

	// 子会话注册
	first, _ := json.Marshal(map[string]interface{}{
		"cmd":    "FirstConnect",
		"ts":     time.Now().Unix(),
		"params": map[string]interface{}{"machine_id": "machine-pipe-c"},
	})
	piv.handleSessionData(aSess, "sess-sp-t", wireFrame(first))
	piv.virtMu.Lock()
	vp := piv.virtualPipes["sess-sp-t"]
	piv.virtMu.Unlock()
	if vp == nil {
		t.Fatalf("虚拟会话应已创建")
	}
	child := vp.Child

	// 上行: child.Write(HeartbeatPing 帧) —— B 会话 runLoop 应消化不崩
	hb, _ := json.Marshal(map[string]interface{}{"cmd": "HeartbeatPing"})
	if _, err := child.Write(wireFrame(hb)); err != nil {
		t.Fatalf("上行写入: %v", err)
	}
	// 上行帧由 B 会话 runLoop 消化; 可观察断言在下行 relay 帧(registr confirm)

	// relay 验证: 子会话注册的 confirm 响应(registerNew 回帧)应经
	// relay → pushData → A 会话 sendFrame → aCli 可读, 且是
	// PivotData(sess-) 混合帧 —— 这同时证明下行 relay 与出站信封注入。
	buf := make([]byte, 64*1024)
	total := 0
	gotSess := false
	var gotHead map[string]interface{}
	var gotBody []byte
	{
		// 单次长 deadline: net.Pipe 的 Read 超时会连带解除对端阻塞中的
		// Write(数据即丢), 短 deadline 轮询会把 confirm 打没 —— 必须让
		// Read 挂到数据到达为止。
		_ = aCli.SetReadDeadline(time.Now().Add(8 * time.Second))
		deadline := time.Now().Add(8 * time.Second)
		for time.Now().Before(deadline) && total < len(buf) {
			n, err := aCli.Read(buf[total:])
			total += n
			t.Logf("LOOP n=%d err=%v total=%d", n, err, total)
			if err != nil {
				break
			}
		head, body, ok := parseHybridTest(buf[:total])
		if !ok {
			continue
		}
		if cmd, _ := head["cmd"].(string); cmd == "PivotData" {
			// pivot_id 在 params(TS 下发)或 data(Agent 上行), 两处兼容
			pid := ""
			for _, sec := range []string{"params", "data"} {
				if m, _ := head[sec].(map[string]interface{}); m != nil {
					if v, _ := m["pivot_id"].(string); v != "" {
						pid = v
						break
					}
				}
			}
			if IsSessionId(pid) {
				gotSess = true
				gotHead = head
				gotBody = body
				break
			}
		}
		}
	}
	if !gotSess {
		t.Logf("DIAG aCli raw(%d): % X", total, buf[:total])
		t.Fatalf("A 方向未收到 sess- PivotData 帧(confirm 应经 relay 透传)")
	}
	if len(gotBody) == 0 {
		t.Fatalf("sess- 帧应有载荷(confirm 落线帧)")
	}
	// 载荷是 B 会话 confirm 的落线帧(4B 头 + JSON), 应能二次解析
	if len(gotBody) >= 4 {
		l := int(binary.BigEndian.Uint32(gotBody[:4]))
		if l > 0 && 4+l <= len(gotBody) {
			var confirm map[string]interface{}
			if err := json.Unmarshal(gotBody[4:4+l], &confirm); err == nil {
				if _, hasStatus := confirm["status"]; !hasStatus {
					t.Fatalf("confirm 帧应含 status: %v", confirm)
				}
			}
		}
	}
	_ = gotHead

	// 级联: child 关闭 → 虚拟会话注销
	child.Close()
	waitCond(t, 2*time.Second, "child 关闭后注销", func() bool {
		piv.virtMu.Lock()
		defer piv.virtMu.Unlock()
		_, exists := piv.virtualPipes["sess-sp-t"]
		return !exists
	})
}
