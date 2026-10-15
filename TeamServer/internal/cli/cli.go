// Package cli 操作员命令行界面(交互式 REPL)。
// 职责: 命令解析/呈现。业务一律经 service 层。
//
// 第 5 批交互重写: 终端层为 chzyer/readline(MIT, 成熟稳定) ——
// reeflective/console 路线报废(黑盒显示状态机, 四轮实测四报: VT 乱码/光标粘行/
// 提示符惰性渲染/最终无响应, 见 git 6e8827a 前后), 原因是它把显示管线的
// 驱动权藏在 internal 包里, 外部无法正确集成。
// chzyer 的模型简单可预测: Readline() 每行返回时终端处于正常模式, 命令输出
// 用普通 fmt 打印完全正确, 下一轮 Readline 重画提示符 —— 无任何隐藏相位。
//
// 已固化并继续有效的成果:
//   - 日志分流: logx 双 sink —— 诊断日志全量进 data/logs/ 日文件, REPL 静默;
//     操作员事件(会话上下线/panic)在每轮读行前 flush 打印(不实时搅输入)。
//   - 会话上下文: use <id> 后 Agent 命令即 CLI 顶级命令(动态挂载进命令树)。
//   - 命令正名: sessions/use/jobs/ls(Sliver 惯例), 旧名全留别名。
//   - 补全: cobra 命令树映射 PrefixCompleter(动态重建, 随 cur 变化)。
//   - Windows: enableVT()(chzyer 同样输出 ANSI)+ 管道/失败降级朴素模式。
//
// 输出纪律: 命令输出走 c.Println/c.Printf 包装(内部 fmt 直写 —— chzyer 模式下
// 正确; 保留包装层以便未来输出策略变化时单点替换)。严禁裸 fmt.Print* 绕过包装。
package cli

import (
	"bufio"
	"errors"
	"fmt"
	"io"
	"os"
	"strings"

	"github.com/chzyer/readline"
	"strconv"

	"github.com/spf13/cobra"

	"cleat/teamserver/internal/eventbus"
	"cleat/teamserver/internal/logx"
	"cleat/teamserver/internal/service"
)

// CLI 操作员界面。
type CLI struct {
	svc *service.Service
	bus *eventbus.Hub // 事件订阅(会话上下线横幅 + 终端输出; 见 cli_term.go)

	cur    int    // 当前选中会话(0=未选)
	exit   bool   // quit/exit 置位, REPL 循环检查
	OnQuit func() // main 注入: quit 时通知主流程优雅关闭(发 errCh 哨兵)

	// ---- 远程终端订阅与 shell 交互模式 ----
	rl      *readline.Instance  // readline 实例(异步输出经其 Write, 保持输入行干净)
	termSID int                 // 当前事件订阅的会话(0=未订阅)
	termCh  chan eventbus.Event // 订阅通道(terminal_output 由此打印)
	shellOn bool                // shell 交互模式: 输入直发 TerminalInput
}

// ---- 输出包装(第 5 批) ----
// chzyer 模式下 Readline() 返回时终端处于正常模式, 命令输出 fmt 直写完全正确;
// 保留包装层作为输出策略单点(未来若换终端层, 只改这里)。

func (c *CLI) Printf(format string, args ...any) {
	fmt.Printf(format, args...)
}

func (c *CLI) Println(args ...any) {
	fmt.Println(args...)
}

// ErrQuit 操作员主动退出哨兵(main 据此走优雅关闭而非"启动失败")。
var ErrQuit = errors.New("operator requested shutdown (CLI quit)")

// New 创建 CLI。
func New(svc *service.Service, bus *eventbus.Hub) *CLI {
	return &CLI{svc: svc, bus: bus}
}

// Run 启动交互式 REPL(阻塞直到退出)。
// 终端层 chzyer/readline: Readline() 每行返回时终端处于正常模式, 命令输出用
// 普通 fmt 打印完全正确, 下一轮重画提示符 —— 无隐藏相位(对比已报废的
// reeflective/console 路线, 见包注释)。
func (c *CLI) Run() {
	defer logx.Guard("cli-run") // G3

	logx.SetStdoutQuiet(true) // 诊断日志只进文件: REPL 界面只留操作员事件

	// Windows: chzyer 同样输出 ANSI, 先开 VT(防重蹈覆辙见 vt_windows.go);
	// 管道/使能失败降级朴素模式。
	if !enableVT() || stdinIsPipe() {
		c.runWithPlain(bufio.NewScanner(os.Stdin))
		return
	}

	// 事件通知: 不实时打(会搅行) —— 进通知队列, REPL 每轮读行前 flush。
	notifyCh := make(chan string, 16)
	logx.SetNotifier(func(msg string) {
		select {
		case notifyCh <- msg:
		default: // 队列满丢弃: 通知不阻断主流程(会话上下线高频时防积压)
		}
	})

	rl, err := readline.NewEx(&readline.Config{
		Prompt:          c.promptText(),
		HistoryFile:     "data/cli_history",
		HistoryLimit:    1000,
		AutoComplete:    dynamicCompleter{c: c},
		InterruptPrompt: "^C",
	})
	if err != nil {
		// readline 初始化失败(无终端等): 降级朴素模式
		logx.SetStdoutQuiet(false)
		c.runWithPlain(bufio.NewScanner(os.Stdin))
		return
	}
	defer func() { _ = rl.Close() }()
	c.rl = rl // 异步终端输出经 rl.Write 打印(自动清行+重绘提示符)

	c.banner()

	for {
		// flush 积压的操作员事件(会话上下线等): 在读行前打印, 提示符随 Readline 重画。
		drained := false
		for !drained {
			select {
			case msg := <-notifyCh:
				fmt.Println(msg)
			default:
				drained = true
			}
		}

		// 提示符随 c.cur 变化: 每轮 SetPrompt(use 切换后立即生效)。
		rl.SetPrompt(c.promptText())

		line, err := rl.Readline()
		if err != nil {
			if err == readline.ErrInterrupt {
				continue // Ctrl-C: 清行不退出(Sliver 同款)
			}
			// Ctrl-D(EOF): shell 交互模式下先脱离 shell(回到主 REPL), 再按才退出进程。
			if c.shellOn && err == io.EOF {
				c.shellExit()
				continue
			}
			break // io.EOF(Ctrl-D / stdin 关闭): 退出
		}
		line = strings.TrimSpace(line)

		// shell 交互模式: 整行直发远程终端(exit 退出), 不走命令树。
		if c.shellOn {
			c.handleShellLine(line)
			continue
		}
		if line == "" {
			continue
		}

		// 每行重建命令树: pflag 状态不残留; Agent 命令随 c.cur 动态挂载。
		root := c.buildCommands()
		root.SetArgs(splitShellLike(line))
		_ = root.Execute() // 错误由各命令 RunE 自行打印(cobra 侧静默)

		if c.exit {
			break
		}
	}

	// 正常退出路径(quit/Ctrl-D): 通知 main 优雅关服。
	logx.SetStdoutQuiet(false)
	logx.SetNotifier(nil)
	if c.OnQuit != nil {
		c.OnQuit()
	}
}

// promptText 提示符文本(随选中会话变化)。
// 防重蹈覆辙: chzyer 每次按键
// 刷新都会重绘 prompt 字符串 —— prompt 内**严禁含前导 \n**(console 时代的遗产,
// chzyer 下每刷一次多滚一行)。命令输出以 \n 结尾, chzyer 下轮直接画 prompt 位置
// 本就正确, 无需任何前导换行。
func (c *CLI) promptText() string {
	if c.shellOn {
		return "[shell ▪ #" + fmt.Sprint(c.cur) + "] » "
	}
	if c.cur > 0 {
		return "[cleat ▪ #" + fmt.Sprint(c.cur) + "] » "
	}
	return "[cleat] » "
}

// dynamicCompleter 动态补全: 每次 Tab 从当前命令树生成候选
// (随 c.cur 变化 —— Agent 命令挂载后立即可补全; 第 5 批)。
type dynamicCompleter struct {
	c *CLI
}

// Do 实现 readline.AutoCompleter: 取光标前最后一个词为前缀,
// 给出命令树中匹配的命令名(含别名)。
func (d dynamicCompleter) Do(line []rune, _ int) (newLine [][]rune, offset int) {
	words := strings.Fields(string(line))
	prefix := ""
	if len(words) > 0 && !strings.HasSuffix(string(line), " ") {
		prefix = strings.ToLower(words[len(words)-1])
	}
	if len(words) > 1 && !strings.HasSuffix(string(line), " ") {
		// 已越过命令词(进入参数区): 不补全参数(目录位置映射本身就是参数便利)。
		return nil, 0
	}
	root := d.c.buildCommands()
	var out [][]rune
	for _, sub := range root.Commands() {
		for _, name := range append([]string{sub.Name()}, sub.Aliases...) {
			if strings.HasPrefix(name, prefix) {
				out = append(out, []rune(strings.TrimPrefix(name, prefix)))
			}
		}
	}
	return out, len([]rune(prefix))
}

// runWithPlain 朴素 REPL 降级(无 VT 终端/管道输入): 无行编辑与补全, 但命令语义一致。
func (c *CLI) runWithPlain(scanner *bufio.Scanner) {
	c.banner()
	for {
		fmt.Print(c.promptText())
		if !scanner.Scan() {
			break
		}
		line := strings.TrimSpace(scanner.Text())

		// shell 交互模式: 整行直发远程终端(exit 退出), 不走命令树。
		if c.shellOn {
			c.handleShellLine(line)
			continue
		}
		if line == "" {
			continue
		}
		if c.exit {
			break
		}
		root := c.buildCommands()
		root.SetArgs(splitShellLike(line))
		_ = root.Execute()
		if c.exit {
			break
		}
	}
	if c.OnQuit != nil {
		c.OnQuit()
	}
}

func (c *CLI) banner() {
	fmt.Println("───────────────────────────────────────────────")
	fmt.Println("  CLEAT · 操作员 CLI")
	fmt.Println("  use <id> 选中会话，Agent 命令直接可用（ls C:\\ · ps · FileMove a b）")
	fmt.Println("  Tab 补全 · Ctrl-R 历史 · help / ? 全部命令 · quit 退出")
	fmt.Println("───────────────────────────────────────────────")
}

// buildCommands 构建完整命令树(每轮重建)。
func (c *CLI) buildCommands() *cobra.Command {
	root := &cobra.Command{
		Use:   "cleat",
		Short: "TeamServer 操作员 CLI",
		Long: `TeamServer 交互式操作员 CLI。
速记: use <id> 选中会话后, Agent 命令直接可用(参数按目录位置映射, 不用写 JSON):
  use 7
  ls C:\            → FileListDir (push)
  ps                → EnumProcess
  FileMove a.txt b.txt → 位置参数按目录映射
Tab 补全命令/参数, Ctrl-R 搜历史。`,
		SilenceErrors: true, // 错误输出由命令 RunE 自管(保持中文提示形态)
		SilenceUsage:  true,
		Args:          cobra.ArbitraryArgs, // 未知命令落 root RunE
		RunE: func(_ *cobra.Command, args []string) error {
			if len(args) > 0 {
				fmt.Println("未知命令:", args[0], "(help)")
			}
			return nil
		},
	}
	// 自定义 help 挂 "?" 别名(原 normalizeAlias 语义)。
	root.SetHelpCommand(&cobra.Command{
		Use:     "help [command]",
		Aliases: []string{"?"},
		Short:   "帮助",
		RunE: func(_ *cobra.Command, args []string) error {
			if len(args) == 0 {
				return root.Help()
			}
			sub, _, err := root.Find(args)
			if err != nil || sub == root {
				return root.Help()
			}
			return sub.Help()
		},
	})

	// ---- 会话管理 ----
	root.AddCommand(&cobra.Command{
		Use:     "sessions",
		Aliases: []string{"list", "ls-list"}, // ls 让位给 Agent 文件枚举(第 5 批), 会话列表面用正名
		Short:   "列出会话(全部, 含离线)",
		RunE: func(_ *cobra.Command, _ []string) error {
			c.list()
			return nil
		},
	})
	root.AddCommand(&cobra.Command{
		Use:     "use <id>",
		Aliases: []string{"select"},
		Short:   "选择目标会话(选中后 Agent 命令直接可用)",
		Args:    cobra.ExactArgs(1),
		RunE: func(_ *cobra.Command, args []string) error {
			id, err := strconvAtoi(args[0])
			if err != nil {
				fmt.Println("用法: use <id>")
				return nil
			}
			c.cur = id
			// 订阅该会话事件: terminal_output 实时打印(shellin/shell 模式的输出可见性)。
			c.subscribeSessionEvents(id)
			if info, ok := c.svc.SessionInfo(id); ok {
				fmt.Printf("目标会话 #%d (%s @ %s)\n", id, info.Hostname, info.RemoteIP)
			} else {
				fmt.Printf("已选择会话 #%d (当前不在线)\n", id)
			}
			return nil
		},
	})
	root.AddCommand(&cobra.Command{
		Use:     "info",
		Aliases: []string{"i"},
		Short:   "显示当前会话信息",
		RunE: func(_ *cobra.Command, _ []string) error {
			c.info()
			return nil
		},
	})
	root.AddCommand(&cobra.Command{
		Use:     "ping",
		Aliases: []string{"p"},
		Short:   "心跳测活",
		RunE: func(_ *cobra.Command, _ []string) error {
			c.ping()
			return nil
		},
	})

	// ---- Agent 命令目录速查 ----
	root.AddCommand(&cobra.Command{
		Use:   "cmd [命令名]",
		Short: "列出全部 Agent 命令(分类) / 查看命令详情和参数",
		RunE: func(_ *cobra.Command, args []string) error {
			c.showCmd(args)
			return nil
		},
	})

	// ---- 命令下发(显式跨会话/通道控制用; 上下文内直接敲命令即可) ----
	root.AddCommand(c.sendCmd("send", "push", "push 直发(--id 跨会话)", []string{"s"}))
	root.AddCommand(&cobra.Command{
		Use:   "pull <cmd> [params]",
		Short: "pull 通道直发(等价 send --pull)",
		RunE: func(_ *cobra.Command, args []string) error {
			id, cmdName, params, _, ok := c.resolveSend(0, 1, "", args)
			if !ok {
				fmt.Println("用法: pull [--id <id>] <cmd> [params] 例如: pull FileListDir C:\\")
				return nil
			}
			c.sendMode("pull", id, cmdName, params)
			return nil
		},
	})
	root.AddCommand(c.sendCmd("task", "task", "异步提交任务(立即返回任务 ID)", []string{"t"}))

	// ---- 任务(一问一答,  用户裁定) ----
	// jobs = 现打一发 JobList 到当前会话, 把 Agent JobCenter 里"此刻跑着什么"
	// 原样带回(不缓存、不落 TS 表 —— 快照即真相, 对齐 CS 式按需查询);
	// all = 逐个在线会话查; --local = 旧的 TS 任务台账(本地表, 与 Agent 无关)。
	jobsCmd := &cobra.Command{
		Use:     "jobs [all]",
		Aliases: []string{"tasks"},
		Short:   "查看 Agent 活跃任务(实时查询; all=全部在线会话, --local=TS 本地台账)",
		RunE: func(cmd *cobra.Command, args []string) error {
			local, _ := cmd.Flags().GetBool("local")
			all := len(args) >= 1 && args[0] == "all"
			switch {
			case local && all:
				c.taskList(0)
			case local:
				c.taskList(c.cur)
			case all:
				c.agentJobsAll()
			default:
				c.agentJobs(c.cur)
			}
			return nil
		},
	}
	jobsCmd.Flags().Bool("local", false, "显示 TS 本地任务台账(不查 Agent)")
	root.AddCommand(jobsCmd)
	root.AddCommand(&cobra.Command{
		Use:   "jobinfo <tid>",
		Short: "查看 TS 任务详情(jobs --local 台账条目)",
		Args:  cobra.MinimumNArgs(1),
		RunE: func(_ *cobra.Command, args []string) error {
			tid, _ := strconvAtoi(args[0])
			c.taskInfo(int64(tid))
			return nil
		},
	})

	// ---- 代理/转发/监听器/模块/产物/迁移/raw/batch(第 4 批已树化, 原样注册) ----
	root.AddCommand(&cobra.Command{
		Use:   "pivot <port>",
		Short: "SOCKS5 单跳出口",
		Args:  cobra.MinimumNArgs(1),
		RunE: func(_ *cobra.Command, args []string) error {
			c.pivotStart(args[0], false, "", 0)
			return nil
		},
	})
	root.AddCommand(&cobra.Command{
		Use:   "pivotchain <port> <entry_host> <entry_port>",
		Short: "多跳链式出口",
		Args:  cobra.MinimumNArgs(3),
		RunE: func(_ *cobra.Command, args []string) error {
			ep, _ := strconvAtoi(args[2])
			c.pivotStart(args[0], true, args[1], ep)
			return nil
		},
	})
	root.AddCommand(&cobra.Command{
		Use:     "entry <port>",
		Aliases: []string{"e"},
		Short:   "Agent 多跳入口(SOCKS5)",
		Args:    cobra.MinimumNArgs(1),
		RunE: func(_ *cobra.Command, args []string) error {
			c.entry(args[0])
			return nil
		},
	})
	root.AddCommand(&cobra.Command{
		Use:   "pivots",
		Short: "列出 SOCKS 出口/反向端口转发/会话借道",
		RunE: func(_ *cobra.Command, _ []string) error {
			for _, l := range c.svc.Pivots() {
				fmt.Println("  -", l)
			}
			for _, info := range c.svc.Rportfwds() {
				fmt.Printf("  - #%d rportfwd :%d -> %s:%d\n", info.SessionID, info.ListenPort, info.TargetHost, info.TargetPort)
			}
			for _, e := range c.svc.PipeEntries() {
				fmt.Printf("  - #%d pipe 入口 %s (key=%s, listener=%s)\n", e.SessionID, e.PipeName, e.Key, e.ListenAddr)
			}
			for _, l := range c.svc.VirtualLinks() {
				fmt.Printf("  - #%d 会话借道 <- %s\n", l.ParentSID, l.PipeName)
			}
			return nil
		},
	})
	root.AddCommand(&cobra.Command{
		Use:   "pipelisten <sid> <listener_addr>",
		Short: "会话借道入口: TS 生成凭据下发 Agent 监听 SMB 管道(子 Agent 经此上线)",
		Args:  cobra.ExactArgs(2),
		RunE: func(_ *cobra.Command, args []string) error {
			sid, err := strconv.Atoi(args[0])
			if err != nil || sid <= 0 {
				return fmt.Errorf("无效会话 id: %s", args[0])
			}
			entry, err := c.svc.PipeEntryAdd("", sid, args[1])
			if err != nil {
				return err
			}
			fmt.Printf("会话借道入口已开启:\n  管道: pipe://<agent-host>/%s\n  密钥: %s\n  加密盒随 listener %s\n",
				entry.PipeName, entry.Key, entry.ListenAddr)
			return nil
		},
	})
	root.AddCommand(&cobra.Command{
		Use:   "pipestop <pipe_name>",
		Short: "关闭会话借道入口",
		Args:  cobra.ExactArgs(1),
		RunE: func(_ *cobra.Command, args []string) error {
			return c.svc.PipeEntryRemove("", args[0])
		},
	})
	root.AddCommand(&cobra.Command{
		Use:   "rportfwd <listen_port> <target_host> <target_port> | stop <listen_port> | list",
		Short: "反向端口转发",
		RunE: func(_ *cobra.Command, args []string) error {
			c.rportfwdCmd(args)
			return nil
		},
	})

	// (第 5 批: 高频快捷命令退役 —— 目录正名 ps/kill/screenshot/regkey/edrscan
	// 已由 agentCmd 动态挂载覆盖, 参数映射按 Agent 实测定义更准。)

	root.AddCommand(&cobra.Command{
		Use:   "migrate <sid> <shc_file> [target]",
		Short: "迁移: shc 注入牺牲进程, 新实例认领原会话",
		RunE: func(_ *cobra.Command, args []string) error {
			c.migrateCmd(args)
			return nil
		},
	})
	root.AddCommand(&cobra.Command{
		Use:   "raw <json>",
		Short: "任意帧透传",
		RunE: func(_ *cobra.Command, args []string) error {
			c.rawFrame(args)
			return nil
		},
	})

	// 监听器
	listenerCmd := &cobra.Command{
		Use:   "listener",
		Short: "动态监听器管理(无参数=列出)",
		RunE: func(_ *cobra.Command, _ []string) error {
			for _, a := range c.svc.ListListenerAddrs() {
				fmt.Println("  -", a)
			}
			return nil
		},
	}
	listenerAdd := &cobra.Command{
		Use:   "add <addr>",
		Short: "添加监听器(默认裸监听; --tls 走默认加密盒)",
		Args:  cobra.ExactArgs(1),
	}
	// 防重蹈覆辙: RunE 必须在变量声明后单独赋值(字面量内自引用编译错)。
	listenerAdd.RunE = func(_ *cobra.Command, args []string) error {
		var err error
		if tlsFlag, _ := listenerAdd.Flags().GetBool("tls"); tlsFlag {
			err = c.svc.AddListener("", "tcp", args[0], "", "")
		} else {
			err = c.svc.AddListenerRaw("", args[0])
		}
		if err != nil {
			fmt.Println("添加监听器失败:", err)
		} else {
			fmt.Println("已添加监听", args[0])
		}
		return nil
	}
	listenerAdd.Flags().Bool("tls", false, "使用默认加密盒与伪装配置(api 同语义)")
	listenerCmd.AddCommand(listenerAdd)
	listenerCmd.AddCommand(&cobra.Command{
		Use:   "rm <addr>",
		Short: "移除监听器",
		Args:  cobra.ExactArgs(1),
		RunE: func(_ *cobra.Command, args []string) error {
			if err := c.svc.RemoveListener("", args[0]); err != nil {
				fmt.Println("移除监听器失败:", err)
			}
			return nil
		},
	})
	root.AddCommand(listenerCmd)

	// 模块/产物/批量
	root.AddCommand(c.moduleTree())
	root.AddCommand(c.artifactTree())
	batchCmd := &cobra.Command{
		Use:   "batch [--id <id>] <file>",
		Short: "按行执行脚本文件到当前/指定会话",
		RunE: func(cmd *cobra.Command, args []string) error {
			id, _ := cmd.Flags().GetInt("id")
			c.batch(id, args)
			return nil
		},
	}
	batchCmd.Flags().Int("id", 0, "目标会话 ID(缺省用 use 的当前会话)")
	root.AddCommand(batchCmd)

	// ---- Agent 命令动态挂载(第 5 批核心体验) ----
	// use <id> 选中会话后, 目录内全部 Agent 命令成为顶级命令:
	//   ls C:\ / FileMove a b / GetSystemInfo —— 等价 send push 当前会话。
	// Agent 命令名首字母大写, 与全小写 CLI 命令天然不冲突。
	if c.cur > 0 {
		for i := range agentCommands {
			d := &agentCommands[i]
			cmd := c.agentCmd(d)
			// shell-start 特化: 启动成功后直接进入交互模式(MSF/Sliver 同款),
			// 输入直发 TerminalInput, exit 退出。见 cli_term.go。
			if d.Name == "TerminalStart" {
				cmd.RunE = func(_ *cobra.Command, _ []string) error {
					c.shellStart()
					return nil
				}
			}
			root.AddCommand(cmd)
		}
		// Agent 文件枚举取 "ls" 正名(Sliver 惯例; CLI 会话列表已让位给 sessions)
		ls := c.agentCmd(&CmdDesc{Name: "FileListDir", Desc: "列出目录内容(ls)"})
		ls.Use = "ls [path]"
		ls.Short = "列出目录内容(Agent)"
		root.AddCommand(ls)
	}

	// ---- 退出 ----
	root.AddCommand(&cobra.Command{
		Use:     "quit",
		Aliases: []string{"exit"},
		Short:   "退出",
		RunE: func(_ *cobra.Command, _ []string) error {
			c.exit = true // 自管循环检测后正常退出 → OnQuit 优雅关服
			return nil
		},
	})

	// Silence 全树继承(cobra 字段不继承): cobra 的 usage 尾部("Use ... --help")
	// 不带换行且绕过 console 行管理 —— 参数错误时 RunE 自己打中文用法, cobra 侧静默。
	var silence func(cmd *cobra.Command)
	silence = func(cmd *cobra.Command) {
		cmd.SilenceUsage = true
		cmd.SilenceErrors = true
		for _, sub := range cmd.Commands() {
			silence(sub)
		}
	}
	silence(root)
	return root
}

// agentCmd 把一个 Agent 命令包装成 cobra 顶级命令(会话上下文内直接可用):
// CLIName 正名(Sliver 惯例) + 协议原名进别名; 位置参数按目录映射为 params
// (不用写 JSON), push 派发到当前会话。
func (c *CLI) agentCmd(d *CmdDesc) *cobra.Command {
	use := d.CLIName
	if len(d.Params) > 0 {
		keys := make([]string, 0, len(d.Params))
		for _, p := range d.Params {
			k := "<" + p.Key + ">"
			if !p.Required {
				k = "[" + p.Key + "]"
			}
			keys = append(keys, k)
		}
		use += " " + strings.Join(keys, " ")
	}
	return &cobra.Command{
		Use:     use,
		Aliases: []string{d.Name}, // 协议原名也可敲(GetSystemInfo 等)
		Short:   d.Desc + " (Agent)",
		RunE: func(_ *cobra.Command, args []string) error {
			if c.cur <= 0 {
				c.Println("未选择会话(先 use <id>)")
				return nil
			}
			// 支持以协议原名调用: findCmd 双向查找(cobra Find 已解析到本命令,
			// 需还原协议名 —— args[0] 是 CLI 正名或别名, 统一按目录映射)。
			cmdName := d.Name
			if argsOverride := paramsFromCatalog(cmdName, args); argsOverride != nil || len(d.Params) == 0 {
				c.sendMode("push", c.cur, cmdName, argsOverride)
				return nil
			}
			// 未知参数形态: 单参走 value fallback
			if len(args) == 1 {
				c.sendMode("push", c.cur, cmdName, map[string]interface{}{"value": args[0]})
				return nil
			}
			c.sendMode("push", c.cur, cmdName, nil)
			return nil
		},
	}
}
