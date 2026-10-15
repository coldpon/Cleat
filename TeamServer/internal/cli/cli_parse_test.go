package cli

import (
	"testing"

	"cleat/teamserver/internal/eventbus"
	"cleat/teamserver/internal/service"
	"cleat/teamserver/internal/session"
	"cleat/teamserver/internal/store"
)

func newTestCLI(t *testing.T) *CLI {
	t.Helper()
	st := store.New()
	svc := service.New(session.NewManager(st, eventbus.New(), nil, nil, false), st, nil, "tok")
	return New(svc, eventbus.New())
}

// send 系解析两段式(scanSendFlags + resolveSend)的语义锁定。
// 用例与原 TestParseCommandSpecHumanFriendlySyntax 逐条对应(cobra 重写时拆两段)。
func TestResolveSendHumanFriendlySyntax(t *testing.T) {
	c := &CLI{cur: 3}
	checks := []struct {
		name       string
		args       []string
		fallbackID int
		wantID     int
		wantCmd    string
		wantMode   string
		wantPath   string
	}{
		{name: "selected-session-shortcut", args: []string{"FileListDir", "C:\\"}, fallbackID: 3, wantID: 3, wantCmd: "FileListDir", wantMode: "push", wantPath: "C:\\"},
		{name: "explicit-id", args: []string{"--id", "7", "FileListDir", "C:\\"}, fallbackID: 3, wantID: 7, wantCmd: "FileListDir", wantMode: "push", wantPath: "C:\\"},
		{name: "pull-mode", args: []string{"--pull", "FileListDir", "C:\\"}, fallbackID: 3, wantID: 3, wantCmd: "FileListDir", wantMode: "pull", wantPath: "C:\\"},
		{name: "json-literal", args: []string{"FileListDir", "--json", `{"path":"C:\temp"}`}, fallbackID: 5, wantID: 5, wantCmd: "FileListDir", wantMode: "push", wantPath: "C:\temp"},
	}

	for _, tc := range checks {
		t.Run(tc.name, func(t *testing.T) {
			id, pull, jsonLit, rest, ok := scanSendFlags(tc.args)
			if !ok {
				t.Fatalf("scanSendFlags returned ok=false")
			}
			if id == 0 {
				id = tc.fallbackID
			}
			gotID, cmd, params, mode, ok2 := c.resolveSend(id, pull, jsonLit, rest)
			if !ok2 {
				t.Fatalf("resolveSend returned ok=false")
			}
			if gotID != tc.wantID || cmd != tc.wantCmd || mode != tc.wantMode {
				t.Fatalf("resolveSend = (%d, %q, %q), want (%d, %q, %q)", gotID, cmd, mode, tc.wantID, tc.wantCmd, tc.wantMode)
			}
			if params == nil {
				t.Fatalf("params should not be nil")
			}
			if got := params["path"]; got != tc.wantPath {
				t.Fatalf("params[path] = %v, want %q", got, tc.wantPath)
			}
		})
	}
}

// 位置参数按目录映射(灵魂功能 "不用写 JSON!")锁定: 类型转换按目录定义。
func TestParamsFromCatalog(t *testing.T) {
	params := paramsFromCatalog("KillProcess", []string{"1234"})
	if params["pid"] != 1234 {
		t.Fatalf("pid 应转 int, got %v(%T)", params["pid"], params["pid"])
	}
	if params := paramsFromCatalog("NoSuchCmd", []string{"x"}); params != nil {
		t.Fatalf("未知命令应返回 nil, got %v", params)
	}
}

// 第 5 批核心体验: 会话上下文内 Agent 命令动态挂载。
// use 前: 目录命令不在树中; use 后: 全部挂载 + ls 正名可用。
func TestAgentCommandsDynamicMount(t *testing.T) {
	c := newTestCLI(t)

	root := c.buildCommands()
	// 注意(ArbitraryArgs): Find 未命中时返回 root 本身且 err=nil —— 用命令名判定而非 err。
	if cmd, _, _ := root.Find([]string{"FileListDir"}); cmd.Name() == "FileListDir" {
		t.Fatal("use 前 Agent 命令不应在树中")
	}

	c.cur = 7
	root = c.buildCommands()
	// ls 正名(FileListDir 已成其别名, cobra Name() 返回 Use 首词 = 正名)
	cmd, _, err := root.Find([]string{"ls"})
	if err != nil || cmd == nil || cmd.Name() != "ls" {
		t.Fatalf("use 后 ls 应在树中, err=%v got=%v", err, cmd)
	}
	if cmd2, _, _ := root.Find([]string{"FileListDir"}); cmd2 == nil || cmd2.Name() != "ls" {
		t.Fatal("协议原名 FileListDir 应作为 ls 的别名可解析")
	}
	if cmd3, _, _ := root.Find([]string{"sessions"}); cmd3 == nil {
		t.Fatal("sessions 应仍可用")
	}
	tree1, tree2 := c.buildCommands(), c.buildCommands()
	if tree1 == tree2 {
		t.Fatal("两次 buildCommands 应返回不同树实例(SetCommands 函数式语义)")
	}
}

// cobra 命令树的别名接线锁定(第 5 批正名: sessions/use/jobs, 旧名留别名)。
func TestCommandTreeAliases(t *testing.T) {
	c := newTestCLI(t)
	root := c.buildCommands()
	root.InitDefaultHelpCmd() // help 命令 Execute 时才挂入, 测试里手动初始化
	aliases := map[string]string{
		"s": "send", "t": "task", "list": "sessions", "p": "ping",
		"?": "help", "e": "entry", "mod": "module",
		"select": "use", "tasks": "jobs", "i": "info",
	}
	cmds := root.Commands()
	for alias, want := range aliases {
		found := false
		for _, sub := range cmds {
			if sub.Name() == want {
				for _, a := range sub.Aliases {
					if a == alias {
						found = true
					}
				}
			}
		}
		if !found {
			t.Errorf("别名 %q 应挂在 %q 上", alias, want)
		}
	}
}

// 未知命令兜底: root RunE 收到未匹配 args(保持"未知命令"行为)。
func TestCommandTreeUnknownCommand(t *testing.T) {
	c := newTestCLI(t)
	root := c.buildCommands()
	root.SetArgs([]string{"nosuchthing", "x"})
	if err := root.Execute(); err != nil {
		t.Fatalf("未知命令不应报错(root RunE 兜底): %v", err)
	}
}

// batch 行内 flags 手扫(无 cobra 场景)。
func TestScanSendFlags(t *testing.T) {
	id, pull, jsonLit, rest, ok := scanSendFlags([]string{"--id", "7", "--pull", "FileListDir", `--json`, `{"a":1}`})
	if !ok || id != 7 || pull != 1 || jsonLit != `{"a":1}` || len(rest) != 1 || rest[0] != "FileListDir" {
		t.Fatalf("scanSendFlags = (%d, %d, %q, %v, %v)", id, pull, jsonLit, rest, ok)
	}
	if _, _, _, rest2, ok2 := scanSendFlags([]string{"Shell", "whoami"}); !ok2 || len(rest2) != 2 {
		t.Fatalf("无 flags 行应全量透传, got %v", rest2)
	}
}

func TestSplitShellLike(t *testing.T) {
	got := splitShellLike(`GetSystemInfo`)
	if len(got) != 1 || got[0] != "GetSystemInfo" {
		t.Fatalf("splitShellLike basic = %#v, want [GetSystemInfo]", got)
	}
	got = splitShellLike(`FileListDir "C:\Program Files"`)
	if len(got) != 2 || got[1] != `C:\Program Files` {
		t.Fatalf("splitShellLike quoted path = %#v, want [FileListDir C:\\Program Files]", got)
	}
}
