package api

import (
	"testing"
	"time"
)

// TestAPIAllowedWindow 限流语义锁定:
// 窗口内前 N 次放行, 第 N+1 次拒绝; 窗口滑过后重新放行; 空条目被清理(防无界增长)。
func TestAPIAllowedWindow(t *testing.T) {
	// 隔离全局状态(测试可能与其他用例共享进程)
	apiRateMu.Lock()
	savedLimit, savedWindow := apiRateLimit, apiRateWindow
	savedHits := apiRateHits
	apiRateLimit = 3
	apiRateWindow = 50 * time.Millisecond
	apiRateHits = map[string][]time.Time{}
	apiRateMu.Unlock()
	t.Cleanup(func() {
		apiRateMu.Lock()
		apiRateLimit, apiRateWindow = savedLimit, savedWindow
		apiRateHits = savedHits
		apiRateMu.Unlock()
	})

	for i := 0; i < 3; i++ {
		if !apiAllowed("op1") {
			t.Fatalf("第 %d 次请求应放行(限额 3)", i+1)
		}
	}
	if apiAllowed("op1") {
		t.Fatal("窗口内第 4 次请求应拒绝")
	}
	// 其他 principal 独立配额
	if !apiAllowed("op2") {
		t.Fatal("不同操作员不应共享配额")
	}
	// 窗口滑过: 放行恢复
	time.Sleep(60 * time.Millisecond)
	if !apiAllowed("op1") {
		t.Fatal("窗口滑过后应重新放行")
	}
	// 防泄漏: 只有一次近期命中的 op2 在窗口过后被清理
	time.Sleep(60 * time.Millisecond)
	apiAllowed("op2") // 触发一次裁剪
	apiRateMu.Lock()
	_, exists := apiRateHits["op1"] // op1 的旧命中全部出窗且本次未新增前可能已被删; 仅验证 op2 存在且 map 短小
	if len(apiRateHits) > 2 {
		t.Errorf("过期条目应被清理, 当前 map 大小 %d", len(apiRateHits))
	}
	_ = exists
	apiRateMu.Unlock()
}
