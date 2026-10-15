package service

// OperatorAdd 令牌最小长度: 短令牌可被暴力枚举,
// 新建操作员令牌强制 ≥16 位; 直接走 store.SaveOperator 的测试播种不受影响。

import "testing"

func TestOperatorAddTokenMinLength(t *testing.T) {
	svc := newPayloadService(t, t.TempDir())

	if err := svc.OperatorAdd("", "weak", "operator", "short", true); err == nil {
		t.Fatal("短令牌(<16)应被拒绝")
	}
	if err := svc.OperatorAdd("", "ok16", "operator", "0123456789abcdef", true); err != nil {
		t.Fatalf("16 位令牌应通过: %v", err)
	}
}
