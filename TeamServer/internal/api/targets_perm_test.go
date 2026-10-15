package api

// 批1 TS-1：读端点越权回归（安全专项 ）。
// 红→绿：修复前 viewer 应得 200（FAIL, 暴露漏洞）；修复后 viewer→403 Denied / operator·admin→200。

import (
	"net/http"
	"net/http/httptest"
	"strconv"
	"testing"

	"cleat/teamserver/internal/apierr"
	"cleat/teamserver/internal/eventbus"
	"cleat/teamserver/internal/service"
	"cleat/teamserver/internal/session"
	"cleat/teamserver/internal/store"
)

// newPermTestServer：播种 admin/operator/viewer 三种操作员。
// fallbackToken 置空——否则兜底 admin 会把 viewer 令牌也解析成 admin，角色就测不出来。
func newPermTestServer(t *testing.T) (*Server, *store.Store) {
	t.Helper()
	st := store.New()
	seed := func(name, token string, role store.OperatorRole) {
		if err := st.SaveOperator(&store.Operator{Username: name, Role: role, Token: token, Active: true}); err != nil {
			t.Fatalf("SaveOperator(%s): %v", name, err)
		}
	}
	seed("root", "tok-admin", store.RoleAdmin)
	seed("oper1", "tok-oper", store.RoleOperator)
	seed("view1", "tok-view", store.RoleViewer)

	bus := eventbus.New()
	mgr := session.NewManager(st, bus, nil, nil, false)
	svc := service.New(mgr, st, nil, "") // fallback 空：角色只由 store 令牌决定
	return New(svc, bus, ""), st
}

func doGet(t *testing.T, url, token string) *http.Response {
	t.Helper()
	req, err := http.NewRequest(http.MethodGet, url, nil)
	if err != nil {
		t.Fatalf("NewRequest: %v", err)
	}
	if token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatalf("Do: %v", err)
	}
	return resp
}

func TestReadEndpointsRBAC(t *testing.T) {
	s, st := newPermTestServer(t)
	// 播种一条任务：让 /api/tasks/{id} 的授权正例返回 200（否则是 404，无法干净区分"授权通过"）。
	tid, err := st.SaveTask(&store.Task{SessionID: 1, Cmd: "Ping", Status: store.TaskPending})
	if err != nil {
		t.Fatalf("SaveTask: %v", err)
	}
	srv := httptest.NewServer(s.Handler())
	defer srv.Close()

	taskPath := "/api/tasks/" + strconv.FormatInt(tid, 10)
	cases := []struct {
		name     string
		token    string
		path     string
		want     int
		wantCode apierr.Code // 仅 want==403 时校验
	}{
		{"viewer_targets_forbidden", "tok-view", "/api/targets", http.StatusForbidden, apierr.CodeDenied},
		{"viewer_task_forbidden", "tok-view", taskPath, http.StatusForbidden, apierr.CodeDenied},
		// 批1补漏(F5): listeners 与 topology 同为"读旁路", 与 targets 同款收敛。
		{"viewer_listeners_forbidden", "tok-view", "/api/listeners", http.StatusForbidden, apierr.CodeDenied},
		{"viewer_topology_forbidden", "tok-view", "/api/topology", http.StatusForbidden, apierr.CodeDenied},
		{"operator_targets_ok", "tok-oper", "/api/targets", http.StatusOK, ""},
		{"operator_task_ok", "tok-oper", taskPath, http.StatusOK, ""},
		{"operator_listeners_ok", "tok-oper", "/api/listeners", http.StatusOK, ""},
		{"operator_topology_ok", "tok-oper", "/api/topology", http.StatusOK, ""},
		{"admin_targets_ok", "tok-admin", "/api/targets", http.StatusOK, ""},
		{"admin_task_ok", "tok-admin", taskPath, http.StatusOK, ""},
		{"admin_listeners_ok", "tok-admin", "/api/listeners", http.StatusOK, ""},
		{"admin_topology_ok", "tok-admin", "/api/topology", http.StatusOK, ""},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			resp := doGet(t, srv.URL+tc.path, tc.token)
			defer func() { _ = resp.Body.Close() }()
			if resp.StatusCode != tc.want {
				t.Fatalf("status = %d, want %d", resp.StatusCode, tc.want)
			}
			if tc.want == http.StatusForbidden {
				p := decodeProblem(t, resp) // 复用 problem_test.go 的断言
				if p.Code != tc.wantCode || p.Domain != apierr.DomainApi {
					t.Fatalf("code/domain = %s/%s, want %s/API", p.Code, p.Domain, tc.wantCode)
				}
			}
		})
	}
}
