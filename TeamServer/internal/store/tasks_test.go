package store

import (
	"path/filepath"
	"testing"
)

// TestStoreTaskLifecycle 验证纯内存模式下任务插入/状态更新/查询。
func TestStoreTaskLifecycle(t *testing.T) {
	st := New()

	id, err := st.SaveTask(&Task{SessionID: 7, Cmd: "ScreenShot", Mode: "push", Status: TaskPending})
	if err != nil {
		t.Fatalf("SaveTask: %v", err)
	}
	if id == 0 {
		t.Fatal("SaveTask 应分配非零 ID")
	}

	st.UpdateTaskStatus(id, TaskCompleted, `{"status":"success"}`)
	tk, ok := st.GetTask(id)
	if !ok {
		t.Fatal("GetTask 应存在")
	}
	if tk.Status != TaskCompleted || tk.SessionID != 7 {
		t.Fatalf("任务状态更新失败: %+v", tk)
	}

	pt := st.PendingTasks(7)
	if len(pt) != 0 {
		t.Fatalf("completed 任务不应在 pending 集合, got %+v", pt)
	}
}

// TestStoreTaskCompensation 验证 pending 任务能被按会话检索(供上线补偿)。
// 在途任务(可能已交付 Agent), 重派即双执行; 旧语义"pending+dispatched 靠调用方过滤"
// 是隐性依赖, 已废除(见 PendingTasks 防重蹈覆辙注)。
func TestStoreTaskCompensation(t *testing.T) {
	st := New()
	_, _ = st.SaveTask(&Task{SessionID: 1, Cmd: "A", Mode: "push", Status: TaskPending})
	_, _ = st.SaveTask(&Task{SessionID: 1, Cmd: "B", Mode: "pull", Status: TaskDispatched})
	_, _ = st.SaveTask(&Task{SessionID: 1, Cmd: "C", Mode: "push", Status: TaskCompleted})
	_, _ = st.SaveTask(&Task{SessionID: 1, Cmd: "E", Mode: "push", Status: TaskTimeout})
	_, _ = st.SaveTask(&Task{SessionID: 1, Cmd: "F", Mode: "push", Status: TaskSuperseded})
	_, _ = st.SaveTask(&Task{SessionID: 2, Cmd: "D", Mode: "push", Status: TaskPending})

	pp := st.PendingTasks(1)
	if len(pp) != 1 {
		t.Fatalf("会话1 应只有 1 个待补派任务(pending-only), got %d", len(pp))
	}
	if pp[0].Cmd != "A" {
		t.Fatalf("待补派任务应为 A(pending), got %s", pp[0].Cmd)
	}
	for _, tk := range pp {
		if tk.SessionID != 1 {
			t.Fatalf("出现他会话任务: %+v", tk)
		}
	}

	// 会话无任务
	if got := st.PendingTasks(99); len(got) != 0 {
		t.Fatalf("会话99 不应有任务")
	}
}

func TestTaskByRequestID(t *testing.T) {
	st := New()
	_, _ = st.SaveTask(&Task{SessionID: 1, Cmd: "X", RequestID: "req-1-abc", Status: TaskPending})
	tk, ok := st.TaskByRequestID("req-1-abc")
	if !ok || tk.Cmd != "X" {
		t.Fatalf("应按 rid 命中, got ok=%v", ok)
	}
	if _, ok := st.TaskByRequestID(""); ok {
		t.Fatal("空 rid 应不命中")
	}
	if _, ok := st.TaskByRequestID("req-1-none"); ok {
		t.Fatal("未知 rid 应不命中")
	}
}

// TestUpdateTaskStatusIf CAS 条件迁移(状态机铁律): 仅当前状态匹配才更新; 失败不改原状态。
func TestUpdateTaskStatusIf(t *testing.T) {
	st := New()
	id, _ := st.SaveTask(&Task{SessionID: 1, Cmd: "X", Status: TaskPending})
	if !st.UpdateTaskStatusIf(id, TaskPending, TaskDispatched, "") {
		t.Fatal("pending→dispatched 应成功")
	}
	// 状态已非 pending: 再走 pending→superseded 应失败且不覆盖
	if st.UpdateTaskStatusIf(id, TaskPending, TaskSuperseded, "should fail") {
		t.Fatal("dispatched 任务的 pending→superseded 迁移应失败")
	}
	tk, _ := st.GetTask(id)
	if tk.Status != TaskDispatched || tk.Result != "" {
		t.Fatalf("CAS 失败后状态应保持 dispatched 且 result 不被污染, got %s/%q", tk.Status, tk.Result)
	}
	if !st.UpdateTaskStatusIf(id, TaskDispatched, TaskTimeout, "delivered, no response") {
		t.Fatal("dispatched→timeout 应成功")
	}
}

// TestSQLiteTaskPersist 验证 SQLite 落盘 + 重启级联恢复(dispatched 重置为 pending)。
func TestSQLiteTaskPersist(t *testing.T) {
	dbPath := filepath.Join(t.TempDir(), "task.db")

	st, err := WithSQLite(dbPath)
	if err != nil {
		t.Fatalf("WithSQLite: %v", err)
	}
	id1, _ := st.SaveTask(&Task{SessionID: 3, Cmd: "X", Mode: "push", Status: TaskPending})
	id2, _ := st.SaveTask(&Task{SessionID: 3, Cmd: "Y", Mode: "pull", Status: TaskDispatched})
	id3, _ := st.SaveTask(&Task{SessionID: 3, Cmd: "Z", Mode: "push", Status: TaskCompleted})
	_ = st.Close()

	// 重启: 新实例从磁盘加载
	st2, err := WithSQLite(dbPath)
	if err != nil {
		t.Fatalf("reopen WithSQLite: %v", err)
	}
	defer func() { _ = st2.Close() }()

	if tk, ok := st2.GetTask(id1); !ok || tk.Status != TaskPending {
		t.Fatalf("任务1 应保持 pending, got %+v", tk)
	}
	// 崩溃重启: 在途(dispatched)任务确证不了, 重置为待派发供补偿
	if tk, ok := st2.GetTask(id2); !ok || tk.Status != TaskPending {
		t.Fatalf("任务2(dispatched) 重启应重置为 pending, got %+v", tk)
	}
	if tk, ok := st2.GetTask(id3); !ok || tk.Status != TaskCompleted {
		t.Fatalf("任务3 应保持 completed, got %+v", tk)
	}
	if all := st2.AllTasks(); len(all) != 3 {
		t.Fatalf("重启应恢复 3 个任务, got %d", len(all))
	}
}
