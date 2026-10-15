package api

import (
	"bytes"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	"cleat/teamserver/internal/eventbus"
	"cleat/teamserver/internal/modules"
	"cleat/teamserver/internal/service"
	"cleat/teamserver/internal/session"
	"cleat/teamserver/internal/store"
)

func newTestServerWithModule(t *testing.T) (*Server, *store.Store, *modules.Repo) {
	t.Helper()
	st := store.New()
	if err := st.SaveOperator(&store.Operator{Username: "admin", Role: store.RoleAdmin, Token: "tok-admin", Active: true}); err != nil {
		t.Fatalf("SaveOperator: %v", err)
	}

	bus := eventbus.New()
	mgr := session.NewManager(st, bus, nil, nil, false)
	svc := service.New(mgr, st, nil, "tok-admin")
	s := New(svc, bus, "tok-admin")

	repo, err := modules.New(t.TempDir())
	if err != nil {
		t.Fatalf("modules.New: %v", err)
	}
	if _, err := repo.Save("demo", []byte("MZ-demo-module")); err != nil {
		t.Fatalf("modules.Save: %v", err)
	}
	svc.SetModuleRepo(repo)
	return s, st, repo
}

// newTestServiceWithModule 构造带模块仓库的 service(不走 HTTP 层, 供 service 级用例)。
func newTestServiceWithModule(t *testing.T) (*service.Service, *store.Store, *modules.Repo) {
	t.Helper()
	st := store.New()
	bus := eventbus.New()
	mgr := session.NewManager(st, bus, nil, nil, false)
	svc := service.New(mgr, st, nil, "tok-admin")
	repo, err := modules.New(t.TempDir())
	if err != nil {
		t.Fatalf("modules.New: %v", err)
	}
	if _, err := repo.Save("demo", []byte("MZ-demo-module")); err != nil {
		t.Fatalf("modules.Save: %v", err)
	}
	svc.SetModuleRepo(repo)
	return svc, st, repo
}

func waitTaskStatus(t *testing.T, st *store.Store, id int64, want store.TaskStatus, timeout time.Duration) {
	t.Helper()
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		task, ok := st.GetTask(id)
		if ok && task.Status == want {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	got, ok := st.GetTask(id)
	if !ok {
		t.Fatalf("task %d not found while waiting for %s", id, want)
	}
	t.Fatalf("task %d status = %s, want %s", id, got.Status, want)
}

func TestSubmitTaskModuleLoadAsyncRegistersTask(t *testing.T) {
	s, st, _ := newTestServerWithModule(t)
	// O-2 后语义: 会话必须存在于持久层 —— 播种一条离线会话记录(异步补派路径的前提)
	st.Put(&store.Record{ID: 1, MachineID: "m-test-1", LastSeen: time.Now()})
	srv := httptest.NewServer(s.Handler())
	defer srv.Close()

	payload := map[string]interface{}{
		"cmd": "ModuleLoad",
		"params": map[string]interface{}{
			"module_id": "demo",
		},
		"mode": "pull",
	}
	body, err := json.Marshal(payload)
	if err != nil {
		t.Fatalf("json.Marshal: %v", err)
	}

	req, err := http.NewRequest("POST", srv.URL+"/api/sessions/1/task", bytes.NewReader(body))
	if err != nil {
		t.Fatalf("NewRequest: %v", err)
	}
	req.Header.Set("Authorization", "Bearer tok-admin")
	req.Header.Set("Content-Type", "application/json")

	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatalf("Do: %v", err)
	}
	defer func() { _ = resp.Body.Close() }()

	if resp.StatusCode != http.StatusOK {
		buf, _ := io.ReadAll(resp.Body)
		t.Fatalf("status = %d, body = %s", resp.StatusCode, string(buf))
	}

	var out struct {
		TaskID int64  `json:"task_id"`
		Status string `json:"status"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&out); err != nil {
		t.Fatalf("Decode response: %v", err)
	}
	if out.Status != "ok" || out.TaskID == 0 {
		t.Fatalf("unexpected response: %+v", out)
	}

	task, ok := st.GetTask(out.TaskID)
	if !ok {
		t.Fatalf("task %d not found", out.TaskID)
	}
	if task.Cmd != "ModuleLoad" || task.SessionID != 1 {
		t.Fatalf("unexpected task: %+v", task)
	}
	if task.RequestID == "" {
		t.Fatal("request_id should not be empty")
	}
	if task.Params == "" {
		t.Fatal("params should be persisted")
	}
	var params map[string]interface{}
	if err := json.Unmarshal([]byte(task.Params), &params); err != nil {
		t.Fatalf("decode task params: %v", err)
	}
	if _, ok := params["sha256"]; !ok {
		t.Fatalf("params should include sha256: %+v", params)
	}
	if task.Mode != "pull" {
		t.Fatalf("mode = %s, want pull", task.Mode)
	}

	// 第 3 批语义: ModuleLoad 并入唯一派发路径后, **离线会话保持 pending**
	// 等上线补偿(此前手搓路径直接回 failed —— 离线≠失败, 是状态机断裂的一部分;
	// 上线后 compensate 携 resolver 字节完整重派)。
	waitTaskStatus(t, st, out.TaskID, store.TaskPending, 2*time.Second)
}

// TestSubmitTaskModuleLoadOfflineStaysPending 第 3 批语义: ModuleLoad 并入唯一派发路径后,
// **离线会话保持 pending** 等上线补偿(此前手搓路径直接回 failed —— 离线≠失败, 是状态机
// 断裂的一部分)。补偿携带 resolver 字节的端到端场景在 session 包
// (TestModuleLoadCompensationCarriesPayload)验证 —— compensate 是私有方法, 补偿触发
// 属会话层内部语义。
func TestSubmitTaskModuleLoadOfflineStaysPending(t *testing.T) {
	svc, st, _ := newTestServiceWithModule(t)
	// O-2 后语义: 播种离线会话记录(有持久记录的"离线"才是异步补派路径的前提;
	// 无记录的未知 sid 现在会被 400 拒绝, 见 TestSubmitTaskUnknownSession)
	st.Put(&store.Record{ID: 42, MachineID: "m-test-42", LastSeen: time.Now()})

	tid, err := svc.SubmitTask("tester", 42, "ModuleLoad",
		map[string]interface{}{"module_id": "demo"}, "pull")
	if err != nil {
		t.Fatalf("SubmitTask: %v", err)
	}
	waitTaskStatus(t, st, tid, store.TaskPending, 2*time.Second)
	// sha256 闭环仍由 service 层前置校验完成
	tk, _ := st.GetTask(tid)
	var params map[string]interface{}
	if err := json.Unmarshal([]byte(tk.Params), &params); err != nil {
		t.Fatalf("decode params: %v", err)
	}
	if _, ok := params["sha256"]; !ok {
		t.Fatalf("params 应含 sha256: %+v", params)
	}
}
