// CLI 辅助: send 系解析核心(两段式)/batch 脚本/通用小工具。
//
// 第 4 批 cobra 重写变更说明:
//   - parseCommandSpec 拆为 scanSendFlags(batch 行内手扫) + resolveSend(flags 解析
//     已由 pflag 原生接管), 位置参数目录映射抽为 paramsFromCatalog(在 cli_catalog.go);
//   - 手写 help()/normalizeAlias 消亡(cobra 自动 help + Aliases 原生接管);
package cli

import (
	"encoding/json"
	"fmt"
	"os"
	"strconv"
	"strings"
	"time"
)

// scanSendFlags 扫描 send 系 flags(--id/-i、--pull/-p、--json/-j)。
// batch 脚本行内没有 cobra, 由本函数手扫; 返回剩余位置参数。
// 语义与原 parseCommandSpec 的 flags 段逐条一致(含非法值整体失败)。
func scanSendFlags(args []string) (id, pull int, jsonLiteral string, rest []string, ok bool) {
	id = 0
	rest = make([]string, 0, len(args))
	for i := 0; i < len(args); i++ {
		switch args[i] {
		case "--id", "-i":
			if i+1 >= len(args) {
				return 0, 0, "", nil, false
			}
			v, err := strconv.Atoi(args[i+1])
			if err != nil {
				return 0, 0, "", nil, false
			}
			id = v
			i++
		case "--pull", "-p":
			pull = 1
		case "--json", "-j":
			if i+1 >= len(args) {
				return 0, 0, "", nil, false
			}
			jsonLiteral = args[i+1]
			i++
		default:
			rest = append(rest, args[i])
		}
	}
	return id, pull, jsonLiteral, rest, true
}

// batch 按行执行脚本文件。每行支持: #/ // 注释、@title 标题、@sleep ms、@pull 前缀、
// 以及与 send 相同的 flags(--id/--pull/--json)与位置参数目录映射。
func (c *CLI) batch(idFlag int, args []string) {
	if len(args) == 0 {
		c.Println("用法: batch [--id <id>] <file>\n示例:\n  batch cases/basic.txt\n  batch --id 7 cases/all.txt")
		return
	}
	id := idFlag
	filePath := ""
	for _, a := range args {
		if filePath != "" {
			c.Println("重复的脚本文件参数:", a)
			return
		}
		filePath = a
	}
	if filePath == "" {
		c.Println("用法: batch [--id <id>] <file>")
		return
	}
	if id <= 0 {
		id = c.cur
	}
	if id <= 0 {
		c.Println("请先 select <id> 或使用 --id <id>")
		return
	}

	b, err := os.ReadFile(filePath)
	if err != nil {
		c.Println("读取批量脚本失败:", err)
		return
	}

	lines := strings.Split(string(b), "\n")
	okCount, failCount := 0, 0
	for idx, raw := range lines {
		line := strings.TrimSpace(raw)
		if line == "" || strings.HasPrefix(line, "#") || strings.HasPrefix(line, "//") {
			continue
		}
		if strings.HasPrefix(line, "@title ") {
			c.Printf("\n=== %s ===\n", strings.TrimSpace(line[len("@title "):]))
			continue
		}
		if strings.HasPrefix(line, "@sleep ") {
			d, err := strconv.Atoi(strings.TrimSpace(line[len("@sleep "):]))
			if err == nil && d > 0 {
				c.Printf("[batch] sleep %dms\n", d)
				time.Sleep(time.Duration(d) * time.Millisecond)
			}
			continue
		}
		if strings.HasPrefix(line, "@pull ") {
			line = "--pull " + strings.TrimSpace(line[len("@pull "):])
		}
		fields := splitShellLike(line)
		if len(fields) == 0 {
			continue
		}
		fID, pull, jsonLit, rest, ok := scanSendFlags(fields)
		if !ok {
			c.Printf("[batch:%d] 跳过: %s\n", idx+1, line)
			failCount++
			continue
		}
		lineID, cmdName, params, mode, ok2 := c.resolveSend(fID, pull, jsonLit, rest)
		if !ok2 {
			c.Printf("[batch:%d] 跳过: %s\n", idx+1, line)
			failCount++
			continue
		}
		_ = fID
		c.Printf("[batch:%d] #%d %s %s\n", idx+1, lineID, mode, cmdName)
		// 统计真实执行结果: 原实现无条件 okCount++,
		// Agent 返回 status=error(未知命令/参数错)也报"成功", 误导排障。
		if c.sendMode(mode, lineID, cmdName, params) {
			okCount++
		} else {
			failCount++
		}
	}
	c.Printf("\n批量执行完成: 成功=%d 失败=%d\n", okCount, failCount)
}

func splitShellLike(s string) []string {
	var out []string
	var cur strings.Builder
	var quote rune
	escaped := false
	flush := func() {
		if cur.Len() > 0 {
			out = append(out, cur.String())
			cur.Reset()
		}
	}
	for _, r := range s {
		if escaped {
			cur.WriteRune(r)
			escaped = false
			continue
		}
		switch r {
		case '\\':
			if quote != 0 {
				cur.WriteRune(r)
				continue
			}
			cur.WriteRune(r)
		case '\'', '"':
			if quote == 0 {
				quote = r
				continue
			}
			if quote == r {
				quote = 0
				continue
			}
			cur.WriteRune(r)
		case ' ', '\t', '\n', '\r':
			if quote == 0 {
				flush()
				continue
			}
			cur.WriteRune(r)
		default:
			cur.WriteRune(r)
		}
	}
	if escaped {
		cur.WriteRune('\\')
	}
	flush()
	return out
}

func parseListenPort(s string) (int, error) {
	s = strings.TrimSpace(s)
	if i := strings.LastIndex(s, ":"); i >= 0 {
		s = s[i+1:]
	}
	p, err := strconv.Atoi(s)
	if err != nil || p <= 0 || p > 65535 {
		return 0, fmt.Errorf("invalid port: %q", s)
	}
	return p, nil
}

func mustJSON(v map[string]interface{}) string {
	b, _ := json.Marshal(v)
	return string(b)
}
