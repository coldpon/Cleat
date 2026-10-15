// Agent 任务查询(jobs 一问一答)的呈现锁定: 过滤/格式化/对账告警。
// 数据形态对齐 Agent JobListHandler 的 SnapshotToJson 输出(冒烟脚本实测同款)。
package cli

import (
	"io"
	"os"
	"strings"
	"testing"
)

// TestPrintAgentJobs 表格化: Main/Infrastructure 过滤、数字/时长格式化、
// roster_mismatch 告警段。stdout 经 os.Pipe 捕获(c.Printf 直写 fmt)。
func TestPrintAgentJobs(t *testing.T) {
	resp := map[string]interface{}{
		"status": "success",
		"data": map[string]interface{}{
			"jobs": []interface{}{
				map[string]interface{}{"id": float64(3), "name": "Shell", "kind": "Service",
					"state": "Running", "age_ms": float64(720000), "timeout_ms": float64(0),
					"resource": "Terminal", "finished": false},
				map[string]interface{}{"id": float64(5), "name": "file-download", "kind": "Task",
					"state": "Completed", "age_ms": float64(3500)},
				map[string]interface{}{"id": float64(9), "name": "JobList", "kind": "Command",
					"state": "Running", "age_ms": float64(4)},
				// 框架类: 应被过滤(操作员不可见)
				map[string]interface{}{"id": float64(1), "name": "cmd-worker", "kind": "Infrastructure",
					"state": "Running", "age_ms": float64(600000)},
				map[string]interface{}{"id": float64(0), "name": "main", "kind": "Main",
					"state": "Running", "age_ms": float64(600000)},
			},
			"roster_mismatch": []interface{}{
				map[string]interface{}{"id": float64(12), "name": "GhostSvc", "kind": "Service"},
			},
		},
	}

	out := captureStdout(t, func() { (&CLI{}).printAgentJobs(resp) })

	for _, want := range []string{"Shell", "Running", "12m00s", "file-download", "3s", "JobList", "对账差集 1 条", "GhostSvc"} {
		if !strings.Contains(out, want) {
			t.Errorf("输出缺少 %q\n--- 输出 ---\n%s", want, out)
		}
	}
	for _, notWant := range []string{"cmd-worker", "Infrastructure"} {
		if strings.Contains(out, notWant) {
			t.Errorf("输出不应包含 %q(框架类应过滤)\n--- 输出 ---\n%s", notWant, out)
		}
	}
}

// TestPrintAgentJobsEmpty 空表与缺失 data 的兜底呈现。
func TestPrintAgentJobsEmpty(t *testing.T) {
	out := captureStdout(t, func() {
		(&CLI{}).printAgentJobs(map[string]interface{}{"data": map[string]interface{}{"jobs": []interface{}{}}})
	})
	if !strings.Contains(out, "无活跃任务") {
		t.Errorf("空表应提示无活跃任务, 实际: %q", out)
	}
	out = captureStdout(t, func() { (&CLI{}).printAgentJobs(map[string]interface{}{"status": "success"}) })
	if !strings.Contains(out, "响应异常") {
		t.Errorf("缺 data 应提示响应异常, 实际: %q", out)
	}
}

// TestAgentJobsNoSession 未选会话时的提示(不走网络, svc 为 nil 安全)。
func TestAgentJobsNoSession(t *testing.T) {
	out := captureStdout(t, func() { (&CLI{}).agentJobs(0) })
	if !strings.Contains(out, "use") {
		t.Errorf("未选会话应提示先 use, 实际: %q", out)
	}
}

// TestHumanAgeMs 时长格式化的边界。
func TestHumanAgeMs(t *testing.T) {
	cases := []struct {
		in   interface{}
		want string
	}{
		{float64(4), "4ms"},
		{float64(3500), "3s"},
		{float64(720000), "12m00s"},
		{float64(3_720_000), "1h02m"},
		{"bad", "-"},
	}
	for _, tc := range cases {
		if got := humanAgeMs(tc.in); got != tc.want {
			t.Errorf("humanAgeMs(%v) = %q, want %q", tc.in, got, tc.want)
		}
	}
}

func captureStdout(t *testing.T, fn func()) string {
	t.Helper()
	old := os.Stdout
	r, w, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	os.Stdout = w
	defer func() { os.Stdout = old }()
	fn()
	_ = w.Close()
	b, _ := io.ReadAll(r)
	return string(b)
}
