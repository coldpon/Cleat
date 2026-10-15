// CLI artifact 命令实现(generate/patch/list)。
// cobra 命令树见 cli_commands.go 的 artifactTree(原手搓 --flag 循环已由 pflag 原生接管);
// 生成/补丁/推导规则经 service(第 2 批结构手术)。
package cli

import (
	"cleat/teamserver/internal/service"
)

// artifactGenerateRun 执行 generate(cobra 已把 flags 组装进 spec)。
func (c *CLI) artifactGenerateRun(spec *service.ArtifactSpec) {
	c.Printf("[artifact] 生成 %s artifact ...\n", spec.Transport)
	result, err := c.svc.ArtifactGenerate(spec)
	if err != nil {
		c.Println("生成失败:", err)
		return
	}
	c.Println("✓ artifact 生成成功")
	c.Printf("  形态:  %s\n", result.Form)
	c.Println("  产物: ", result.BinPath)
	c.Println("  启动: ", result.RunLine)
	if result.X25519Key != "" {
		if result.KeyReused {
			c.Println("  密钥:  复用长期 X25519 密钥对(TeamServer 用 -x25519-key 注入同一私钥)")
		} else {
			c.Println("  密钥:  新生成 X25519 长期密钥对(已落盘供后续样本复用)")
		}
		c.Printf("  server 私钥(保存): %s\n", result.X25519Key)
		c.Println("  密钥对文件: ", result.KeyStore)
	}
	for _, w := range result.Warnings {
		c.Println("  [告警]", w)
	}
	if hint, applicable := c.svc.ArtifactUploadHint(spec); applicable {
		if hint != "" {
			c.Println("  提示: 未给 --upload-url, 已按 beacon 默认约定推导为:", hint)
		} else {
			c.Println("  提示: 未给 --upload-url 且 URL 非 /beacon/poll 约定路径, 上传将回退到 GET 同 URL(上报帧会被 server 当探活丢弃); 请显式 --upload-url")
		}
	}
}

// payloadGenerateRun 执行模板烧录生成样本(零工具链, 与 Web /api/payloads/generate 同路径)。
//  修复: 此前 CLI 只有现场编译路径(默认关闭), 操作员在 CLI 内无法出样本,
// 错误信息让用户"走 /api/payloads/generate" —— 现默认走生成器路径, 工作流闭环。
func (c *CLI) payloadGenerateRun(spec *service.PayloadSpec) {
	transport := spec.Transport
	if transport == "" {
		transport = "tcp"
	}
	c.Printf("[artifact] 模板烧录生成 %s 样本 ...\n", transport)
	result, err := c.svc.GeneratePayload("", spec) // op="": 本地 CLI, 不落审计
	if err != nil {
		c.Println("生成失败:", err)
		return
	}
	c.Println("✓ 样本生成成功")
	c.Printf("  名称: %s (%s)\n", result.Name, result.Form)
	c.Printf("  产物: %s\n", result.BinPath)
	c.Printf("  运行: %s  (配置已烧入配置块, 零参数直接运行)\n", result.RunLine)
	c.Printf("  下载: %s\n", result.DownloadURL)
	if result.BuiltAt != "" {
		c.Printf("  构建: %s\n", result.BuiltAt)
	}
	for _, w := range result.Warnings {
		c.Println("  [告警]", w)
	}
}

// artifactPatchRun 执行 patch(cobra 已把 flags 组装进 cfg)。
func (c *CLI) artifactPatchRun(tmplPath, outPath string, cfg map[string]any) {
	if outPath == "" {
		c.Println("缺少 --out <输出文件>")
		return
	}
	if len(cfg) == 0 {
		c.Println("未提供任何配置(模板将被原样输出)")
		return
	}
	if err := c.svc.ArtifactPatch(tmplPath, outPath, cfg); err != nil {
		c.Println("patch 失败:", err)
		return
	}
	c.Println("已生成样本:", outPath, "(配置:", cfg["transport"], cfg["host"], ")")
}
