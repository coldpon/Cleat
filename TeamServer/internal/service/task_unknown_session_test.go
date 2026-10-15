package service

// SubmitTask 对不存在会话的拒绝(O-2, TS 收尾真机 E2E 发现):
// 原先对未知 sid 照常落 pending —— 永远无人派发, 操作员视角"命令失踪"。

import (
	"errors"
	"testing"
)

func TestSubmitTaskUnknownSession(t *testing.T) {
	svc := newPayloadService(t, t.TempDir())

	_, err := svc.SubmitTask("", 424242, "Ping", nil, "")
	var se *Error
	if err == nil || !errors.As(err, &se) || se.Class != ErrInvalid {
		t.Fatalf("不存在会话应返回 400 invalid, got %v", err)
	}
}
