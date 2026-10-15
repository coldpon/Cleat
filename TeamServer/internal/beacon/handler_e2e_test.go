package beacon

import (
	"bytes"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"cleat/teamserver/internal/crypto"
	"cleat/teamserver/internal/eventbus"
	"cleat/teamserver/internal/session"
	"cleat/teamserver/internal/store"
)

// TestHandlerEndToEnd S5 检查点: PSK 模式下注册 → GET 取件 → 护栏的完整链路。
// 覆盖验收判据(计划):
//   GET 既保活又取件(带身份块取回 outbox 帧; 无 m= 仍 200 空);
//   POST 响应带载荷的兼容期行为(A 批切换点才收缩);
//   非法身份块 400(不静默降级)。
func TestHandlerEndToEnd(t *testing.T) {
	cbox, err := crypto.New(strings.Repeat("cd", 32))
	if err != nil {
		t.Fatalf("crypto.New: %v", err)
	}
	mgr := session.NewManager(store.New(), eventbus.New(), cbox, nil, false)
	h := New(mgr, cbox, nil)
	upload := h.Upload()
	poll := h.Poll()

	// ---- 1) POST FirstConnect: 注册; confirm 经 POST 响应体带回 ——
	// **注册帧是 S2.4' 纯上行的唯一例外**: Agent
	// 注册前无 mid 无 sid, GET 只能是裸探活, confirm 无 GET 路径可取。
	fc := fmt.Sprintf(`{"cmd":"FirstConnect","ts":%d,"params":{"machine_id":"e2e-m1","hostname":"e2e"}}`,
		time.Now().Unix())
	enc, err := cbox.Encrypt([]byte(fc))
	if err != nil {
		t.Fatalf("Encrypt(fc): %v", err)
	}
	rec := httptest.NewRecorder()
	upload(rec, httptest.NewRequest("POST", "/upload", bytes.NewReader(enc)))
	if rec.Code != http.StatusOK || rec.Body.Len() == 0 {
		t.Fatalf("注册帧 POST 应 200 且带回 confirm, got code=%d len=%d",
			rec.Code, rec.Body.Len())
	}
	plain, err := cbox.Decrypt(rec.Body.Bytes())
	if err != nil {
		t.Fatalf("confirm 解密: %v", err)
	}
	var confirm struct {
		Status string `json:"status"`
		Data   struct {
			SessionID int `json:"session_id"`
		} `json:"data"`
	}
	if err := json.Unmarshal(plain, &confirm); err != nil || confirm.Data.SessionID == 0 {
		t.Fatalf("confirm 帧不符: %s (err=%v)", plain, err)
	}
	sid := confirm.Data.SessionID

	// ---- 1b) GET sid 态: outbox 应为空(confirm 已随注册响应交付, 无补发) ----
	sidBlk, _ := json.Marshal(IdentityBlock{SID: sid, TS: time.Now().Unix(),
		Nonce: strings.Repeat("6d", 16)})
	sidCt, _ := cbox.Encrypt(sidBlk)
	sidRec := httptest.NewRecorder()
	poll(sidRec, httptest.NewRequest("GET", "/check?m="+b32url.EncodeToString(sidCt), nil))
	if sidRec.Code != http.StatusOK || sidRec.Body.Len() != 0 {
		t.Fatalf("确认后 GET 应 200 空(confirm 无补发), got code=%d len=%d body=%s",
			sidRec.Code, sidRec.Body.Len(), sidRec.Body.String())
	}

	// ---- 2) 派发任务 → GET 身份块取回命令帧 ----
	if _, err := mgr.APISubmitTask(sid, "Ping", map[string]interface{}{"n": 1}, "pull"); err != nil {
		t.Fatalf("APISubmitTask: %v", err)
	}
	blk, _ := json.Marshal(IdentityBlock{SID: sid, TS: time.Now().Unix(), Nonce: strings.Repeat("7a", 16)})
	ct, err := cbox.Encrypt(blk)
	if err != nil {
		t.Fatalf("Encrypt(block): %v", err)
	}
	getReq := httptest.NewRequest("GET", "/check?m="+b32url.EncodeToString(ct), nil)
	rec2 := httptest.NewRecorder()
	poll(rec2, getReq)
	if rec2.Code != http.StatusOK {
		t.Fatalf("GET 取件应 200, got %d", rec2.Code)
	}
	if rec2.Body.Len() == 0 {
		t.Fatal("GET 取件应带回任务命令帧")
	}
	cmdPlain, err := cbox.Decrypt(rec2.Body.Bytes())
	if err != nil {
		t.Fatalf("GET 响应解密: %v", err)
	}
	if !strings.Contains(string(cmdPlain), `"cmd":"Ping"`) {
		t.Fatalf("GET 取回的应是 Ping 命令帧, got %s", cmdPlain)
	}

	// ---- 2b) POST 纯上行复核: 任何上传的响应体恒空 ----
	up, _ := json.Marshal(map[string]interface{}{"cmd": "HeartbeatPing", "ts": time.Now().Unix(),
		"params": map[string]interface{}{"session_id": sid}, "request_id": "hb-1"})
	upCt, _ := cbox.Encrypt(up)
	upRec := httptest.NewRecorder()
	upload(upRec, httptest.NewRequest("POST", "/upload", bytes.NewReader(upCt)))
	if upRec.Code != http.StatusOK || upRec.Body.Len() != 0 {
		t.Fatalf("纯上行 POST 响应体应恒空, got code=%d len=%d",
			upRec.Code, upRec.Body.Len())
	}

	// ---- 3) 再取一次: outbox 空 → 200 空 ----
	rec3 := httptest.NewRecorder()
	// 换 nonce(同块重放会被守卫拒绝, 而这里是"合法的第二次取件")
	blk2, _ := json.Marshal(IdentityBlock{SID: sid, TS: time.Now().Unix(), Nonce: strings.Repeat("8b", 16)})
	ct2, _ := cbox.Encrypt(blk2)
	poll(rec3, httptest.NewRequest("GET", "/check?m="+b32url.EncodeToString(ct2), nil))
	if rec3.Code != http.StatusOK || rec3.Body.Len() != 0 {
		t.Fatalf("空 outbox 应 200 空, got %d body=%q", rec3.Code, rec3.Body.String())
	}

	// ---- 4) 护栏: 无 m= → 200 空(探活); 非法 m= → 400 ----
	rec4 := httptest.NewRecorder()
	poll(rec4, httptest.NewRequest("GET", "/check", nil))
	if rec4.Code != http.StatusOK || rec4.Body.Len() != 0 {
		t.Fatalf("无 m= 探活应 200 空, got %d", rec4.Code)
	}
	rec5 := httptest.NewRecorder()
	poll(rec5, httptest.NewRequest("GET", "/check?m=!!bad!!", nil))
	if rec5.Code != http.StatusBadRequest {
		t.Fatalf("非法身份块应 400, got %d", rec5.Code)
	}
}
