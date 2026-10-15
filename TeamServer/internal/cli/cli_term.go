// 服务端实现 —— 远程终端: 事件订阅(terminal_output)与 MSF 式 shell 交互模式。
//
// 背景: CLI 的 shellin 发命令后只有 "Command executed",
// 命令的实际输出从未出现 —— CLI 从未订阅 eventbus 的 terminal_output
// (该事件此前只被 Web SSE 消费; CLI 的 logx notifier 只覆盖会话上下线)。
//
// 本文件补上两条链路:
//  1. 事件订阅: use <id> 即订阅当前会话事件, terminal_output 实时打印
//     (readline 模式经 rl.Write, 写前自动清行、写后自动重绘提示符 —— 官方异步输出通道)。
//  2. shell 交互模式(MSF/Sliver 同款): shell-start 成功后进入, 输入直发 TerminalInput,
//     输出经事件订阅实时到达; exit 退出并关闭远程 shell。
//
// 限制说明: TerminalInput 通道不带信号语义, shell 模式下 Ctrl-C 只清行
// (不会向远程进程发 SIGINT; 远端卡死命令请用 shellclose/重启恢复)。
package cli

import (
	"fmt"
	"strings"

	"cleat/teamserver/internal/eventbus"
)

// subscribeSessionEvents 切换当前订阅的会话事件通道(use / shellStart 调用)。
// 幂等: 同一会话重复调用不重建; 切换会话时退订旧的(close 通道 → 旧 pump 退出)。
func (c *CLI) subscribeSessionEvents(sid int) {
	if c.bus == nil || sid <= 0 {
		return
	}
	if c.termSID == sid && c.termCh != nil {
		return
	}
	if c.termCh != nil {
		c.bus.Unsubscribe(c.termSID, c.termCh)
		c.termCh = nil
	}
	c.termSID = sid
	ch := c.bus.SubscribeSession(sid)
	c.termCh = ch
	go c.pumpSessionEvents(sid, ch)
}

// pumpSessionEvents 消费订阅通道: 只打印 terminal_output, 其余事件丢弃。
// 必须持续消费: 订阅通道缓冲有限(subBufferSize), 不消费会拖慢/触发 eventbus 丢弃。
func (c *CLI) pumpSessionEvents(sid int, ch chan eventbus.Event) {
	for ev := range ch {
		if ev.Type != eventbus.EventTerminalOutput {
			continue
		}
		m, ok := ev.Data.(map[string]interface{})
		if !ok {
			continue
		}
		out, _ := m["output"].(string)
		if out == "" {
			continue
		}
		c.termPrint(out)
	}
	_ = sid // 预留给未来的按会话输出前缀
}

// termPrint 异步输出终端数据:
//   - readline 模式: 经 rl.Write —— 内部会先清除当前输入行、写完自动重绘提示符
//     (chzyer 官方异步输出通道, "readline will refresh automatic when write through Stdout()");
//   - 管道/降级模式(无 rl): 直接写 stdout。
func (c *CLI) termPrint(s string) {
	if c.rl != nil {
		_, _ = c.rl.Write([]byte(s))
		return
	}
	fmt.Print(s)
}

// setPrompt 更新 readline 提示符(shell 模式进入/退出时即时生效)。
func (c *CLI) setPrompt() {
	if c.rl != nil {
		c.rl.SetPrompt(c.promptText())
	}
}

// shellStart shell-start 的特化实现: 启动远程终端, 成功后进入交互模式(MSF 式)。
// 启动失败(会话不在线/Agent 拒绝)时留在主 REPL 并给出原因。
func (c *CLI) shellStart() {
	if c.cur <= 0 {
		c.Println("未选择会话(先 use <id>)")
		return
	}
	// 先订阅再启动: 保证 "Shell started" 及其后的输出不丢。
	c.subscribeSessionEvents(c.cur)
	sid := c.cur
	resp, err := c.svc.Command("", sid, "TerminalStart", nil, "push")
	if err != nil {
		c.Println("启动 shell 失败:", err)
		return
	}
	if resp != nil {
		if st, _ := resp["status"].(string); st == "error" {
			msg, _ := resp["message"].(string)
			if msg == "" {
				msg = "unknown"
			}
			c.Println("启动 shell 失败:", msg)
			return
		}
	}
	c.shellOn = true
	c.setPrompt()
	c.Printf("[shell] 已进入会话 #%d 的远程终端: 输入直发远程 shell; 输入 exit 退出(将关闭远程 shell)\n", sid)
}

// shellExit 退出交互模式并关闭远程 shell。
func (c *CLI) shellExit() {
	c.shellOn = false
	sid := c.cur
	c.setPrompt()
	c.Println("[shell] 已退出交互模式; 正在关闭远程 shell ...")
	go func() {
		if _, err := c.svc.Command("", sid, "TerminalClose", nil, "push"); err != nil {
			c.termPrint(fmt.Sprintf("[shell] 关闭远程 shell 失败: %v\n", err))
		}
	}()
}

// handleShellLine 处理 shell 交互模式的输入行:
//   - exit: 退出模式并关闭远程 shell(与 MSF 同款; 在本地拦截, 不发给远程);
//   - 空行: 忽略;
//   - 其余: 异步直发 TerminalInput(不阻塞下一行输入; 输出由事件 pump 实时打印)。
func (c *CLI) handleShellLine(line string) {
	trimmed := strings.TrimSpace(line)
	switch trimmed {
	case "exit":
		c.shellExit()
		return
	case "":
		return
	}
	sid := c.cur
	go func() {
		if _, err := c.svc.Command("", sid, "TerminalInput",
			map[string]interface{}{"command": line}, "push"); err != nil {
			c.termPrint(fmt.Sprintf("\n[shell] 发送失败: %v\n", err))
		}
	}()
}
