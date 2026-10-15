// 操作员管理用例。搬运自 api/operators.go(weakPasswords 表随行)。
package service

import (
	"cleat/teamserver/internal/auth"
	"cleat/teamserver/internal/logx"
	"cleat/teamserver/internal/store"
)

// weakPasswords 常见默认/弱口令拒绝表。配置模板默认口令 admin123
// 亦在列 —— 部署后首次改密即被强制换掉。搬运自 api/operators.go。
var weakPasswords = map[string]bool{
	"admin123":  true,
	"12345678":  true,
	"123456789": true,
	"password":  true,
	"password1": true,
	"qwertyui":  true,
	"11111111":  true,
	"88888888":  true,
}

// OperatorList 操作员列表(viewer 不可见敏感令牌的裁剪由认证器完成)。搬运自 api.listOperators。
func (s *Service) OperatorList() []auth.OpSummary { return s.authH.ListOperators() }

// OperatorAdd 新增/更新操作员。搬运自 api.addOperator 主体。
func (s *Service) OperatorAdd(op, username, role, token string, active bool) error {
	o := &store.Operator{Username: username, Role: store.OperatorRole(role), Token: token, Active: active}
	if !o.Valid() {
		return invalid("操作员需有用户名/合法角色(admin|operator|viewer)/令牌")
	}
	// A1-7 补强(TS-5): 令牌最小长度 —— 短令牌可被暴力枚举(≥16 对齐常见 API key 下限)
	if len(token) < 16 {
		return invalid("令牌至少 16 位")
	}
	if err := s.st.SaveOperator(o); err != nil {
		return err
	}
	if op != "" {
		logx.Printf("[audit] op=%s 新增操作员 %s(%s)\n", op, o.Username, o.Role)
	}
	return nil
}

// OperatorRemove 删除操作员。搬运自 api.removeOperator 主体。
func (s *Service) OperatorRemove(op, name string) {
	s.st.RemoveOperator(name)
	if op != "" {
		logx.Printf("[audit] op=%s 删除操作员 %s\n", op, name)
	}
}

// OperatorToggle 启用/禁用操作员, 返回翻转后的状态。搬运自 api.toggleOperator 主体。
func (s *Service) OperatorToggle(op, name string) (bool, error) {
	o, ok := s.st.Operator(name)
	if !ok {
		return false, notFound("操作员不存在")
	}
	o.Active = !o.Active
	if err := s.st.SaveOperator(o); err != nil {
		return false, err
	}
	state := "禁用"
	if o.Active {
		state = "启用"
	}
	if op != "" {
		logx.Printf("[audit] op=%s %s 操作员 %s\n", op, state, name)
	}
	return o.Active, nil
}

// ChangePassword 修改操作员密码。搬运自 api.changePassword 主体(权限 403 判断留在入口层)。
// admin 仅影响"是否校验旧密码"的分支, 由入口层传 identity.IsAdmin()。
func (s *Service) ChangePassword(op, name, oldPw, newPw string, admin bool) error {
	// 密码强度: 曾只校验 len>=4, 弱口令可直接爆破。
	// 最低 8 位 + 拒绝常见默认口令。
	if len(newPw) < 8 {
		return invalid("密码至少 8 位")
	}
	if weakPasswords[newPw] {
		return invalid("密码过于常见, 请更换")
	}
	o, ok := s.st.Operator(name)
	if !ok {
		return notFound("操作员不存在")
	}
	// 非 admin 需验证旧密码
	if !admin {
		if o.PasswordHash == "" {
			return invalid("该帐号未设密码, 请联系管理员")
		}
		if err := auth.ComparePassword(o.PasswordHash, oldPw); err != nil {
			return authFail("旧密码错误")
		}
	}
	hash, err := auth.HashPassword(newPw)
	if err != nil {
		return fail("哈希失败")
	}
	o.PasswordHash = hash
	if err := s.st.SaveOperator(o); err != nil {
		return err
	}
	if op != "" {
		logx.Printf("[audit] op=%s 修改密码 target=%s\n", op, name)
	}
	return nil
}
