package beacon

import (
	"testing"
)

// TestFrameSessionIDHybrid 锁定"混合帧必须先按 '\0' 拆头再解析"这条契约。
//
// 回归背景: 此前 frameSessionID 把**整帧**喂给 json.Unmarshal,
// 而 Go 的 encoding/json 对顶层裸 NUL 是**报错**(不是读到第一个 JSON 值就停) ——
// 于是所有流数据混合帧(DesktopStreamData 等)在这里恒返回 0, BeaconFrame 建临时
// 会话, ResolveIdentity 走 default 分支匿名注册, 一帧一个新会话
// (实测 HTTPS poll 下一次远程桌面开出几十个匿名会话)。
//
// 形状判定现统一走 session.SplitFrame(全仓唯一拆帧权威)。附带收紧: 头声明
// body_size 却不带体(声明与实际矛盾)是坏帧, 恒校验拒绝 —— 旧实现静默放行。
func TestFrameSessionIDHybrid(t *testing.T) {
	head := `{"cmd":"DesktopStreamData","session_id":7,"ts":1758620000,` +
		`"data":{"type":"desktop_full","format":"jpeg"},"body_size":4}`

	hybrid := append([]byte(head), 0)
	hybrid = append(hybrid, 0xff, 0xd8, 0xff, 0xe0) // 裸 JPEG 头 + 二进制体

	if got := frameSessionID(hybrid); got != 7 {
		t.Fatalf("混合帧应取到顶层 session_id=7, got %d", got)
	}
	// 纯 JSON 帧(无二进制体): body_size 不声明(空体混合帧的物理形态), 行为不变
	plain := `{"cmd":"DesktopStreamData","session_id":7,"ts":1758620000,` +
		`"data":{"type":"desktop_full","format":"jpeg"}}`
	if got := frameSessionID([]byte(plain)); got != 7 {
		t.Fatalf("纯 JSON 帧应取到 session_id=7, got %d", got)
	}
	// 坏帧: 头声明 body_size=4 却无体 —— 声明与实际矛盾, 恒校验拒绝(返回 0)
	if got := frameSessionID([]byte(head)); got != 0 {
		t.Fatalf("声明 body_size 却无体的坏帧应拒绝(返回 0), got %d", got)
	}
}

// TestFrameSessionIDNested 验证回退次序: 顶层 → params → data 仍然成立
// (截断逻辑不应破坏原有的多位置查找)。
func TestFrameSessionIDNested(t *testing.T) {
	// 顶层无 sid, params 里有
	p := `{"cmd":"HeartbeatPing","params":{"session_id":11},"body_size":2}`
	hybridP := append([]byte(p), 0)
	hybridP = append(hybridP, 0x01, 0x02)
	if got := frameSessionID(hybridP); got != 11 {
		t.Fatalf("params.session_id 应回退取到 11, got %d", got)
	}

	// 顶层与 params 都无, data 里有
	d := `{"cmd":"PivotData","data":{"session_id":13},"body_size":2}`
	hybridD := append([]byte(d), 0)
	hybridD = append(hybridD, 0x03, 0x04)
	if got := frameSessionID(hybridD); got != 13 {
		t.Fatalf("data.session_id 应回退取到 13, got %d", got)
	}

	// 完全无身份 → 0(首连 FirstConnect 的正常形态)
	if got := frameSessionID([]byte(`{"cmd":"FirstConnect","params":{}}`)); got != 0 {
		t.Fatalf("无 session_id 应返回 0, got %d", got)
	}
}
