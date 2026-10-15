package session

import (
	"bytes"
	"encoding/json"
	"testing"
)

// wire.go 的形状契约锁定用例: SplitFrame/BuildFrame 是全仓唯一拆/装权威,
// 这些断言钉死"纯 JSON = 空体混合帧"的概念模型与字节级行为。

func TestSplitFrame纯JSON(t *testing.T) {
	raw := []byte(`{"cmd":"Poll","ts":1}`)
	header, body, err := SplitFrame(raw)
	if err != nil {
		t.Fatalf("纯 JSON 帧不应报错: %v", err)
	}
	if !bytes.Equal(header, raw) {
		t.Fatalf("header 应为整帧, got %s", header)
	}
	if body != nil {
		t.Fatalf("纯 JSON 帧 body 应为 nil, got %d 字节", len(body))
	}
}

func TestSplitFrame混合帧(t *testing.T) {
	head := []byte(`{"cmd":"ScreenShot","body_size":5,"ts":1}`)
	bin := []byte{0xff, 0x00, 0xd8, 0x00, 0xe0} // 体里含裸 NUL: JSON 头不可能出现
	raw := append(append([]byte{}, head...), 0)
	raw = append(raw, bin...)

	header, body, err := SplitFrame(raw)
	if err != nil {
		t.Fatalf("混合帧不应报错: %v", err)
	}
	if !bytes.Equal(header, head) {
		t.Fatalf("header 应恰为分隔符前缀, got %s", header)
	}
	if !bytes.Equal(body, bin) {
		t.Fatalf("二进制体必须完整保留(含裸 NUL), got %v", body)
	}
}

func TestSplitFrameBodySize不符拒绝(t *testing.T) {
	raw := []byte(`{"cmd":"X","body_size":99}` + "\x00" + "abcd")
	if _, _, err := SplitFrame(raw); err == nil {
		t.Fatalf("头声明 99 实际 4 应报错")
	}
	// 声明 body_size 却无体(矛盾帧)同样拒绝
	if _, _, err := SplitFrame([]byte(`{"cmd":"X","body_size":1}`)); err == nil {
		t.Fatalf("声明 body_size 却无体应报错")
	}
}

func TestSplitFrameBodySize零允许(t *testing.T) {
	// 声明 body_size=0 且无体: 声明与实际一致(0==0), 放行
	header, body, err := SplitFrame([]byte(`{"cmd":"X","body_size":0}`))
	if err != nil {
		t.Fatalf("body_size=0 且无体应放行: %v", err)
	}
	if body != nil {
		t.Fatalf("body 应为 nil")
	}
	if header == nil {
		t.Fatalf("header 不应为 nil")
	}
}

func TestSplitFrame坏帧(t *testing.T) {
	// 非 JSON 且无分隔符
	if _, _, err := SplitFrame([]byte("garbage bytes")); err == nil {
		t.Fatalf("垃圾帧应报错")
	}
	// 以 \0 开头(空头)
	if _, _, err := SplitFrame([]byte("\x00body")); err == nil {
		t.Fatalf("空头帧应报错")
	}
	// 空 bytes
	if _, _, err := SplitFrame(nil); err == nil {
		t.Fatalf("空帧应报错")
	}
}

func TestSplitFrame头转义NUL不受干扰(t *testing.T) {
	// nlohmann/encoding-json 对字符串中的 NUL 输出转义序列(六个字面字符),
	// 字节流不含裸 0x00 —— 判据(找第一个 0x00)不受干扰, 仍按纯 JSON 处理。
	// 用 Marshal 动态构造, 避免源码手写转义歧义。
	payload, err := json.Marshal(map[string]string{"cmd": "A", "note": string([]byte{0})})
	if err != nil {
		t.Fatalf("Marshal 不应失败: %v", err)
	}
	if bytes.IndexByte(payload, 0) != -1 {
		t.Fatalf("前置验证: Marshal 产物不应含裸 NUL")
	}
	header, body, err := SplitFrame(payload)
	if err != nil {
		t.Fatalf("含转义 NUL 的合法 JSON 应放行: %v", err)
	}
	if body != nil {
		t.Fatalf("应为纯 JSON 帧, got %d 字节体", len(body))
	}
	if !bytes.Equal(header, payload) {
		t.Fatalf("header 应为整帧")
	}
}

func TestBuildFrame空体输出裸JSON(t *testing.T) {
	head := []byte(`{"cmd":"Poll","ts":1}`)
	out, err := BuildFrame(head, nil)
	if err != nil {
		t.Fatalf("空体构造不应报错: %v", err)
	}
	if !bytes.Equal(out, head) {
		t.Fatalf("空体输出应为裸 JSON(无分隔符), got %q", out)
	}
	if bytes.IndexByte(out, 0) != -1 {
		t.Fatalf("空体输出不得含 \\0 分隔符")
	}
}

func TestBuildFrame带体恒写BodySize(t *testing.T) {
	head := []byte(`{"cmd":"FileUploadData","body_size":999,"ts":1}`) // 手写的错误值
	bin := []byte{1, 2, 3}
	out, err := BuildFrame(head, bin)
	if err != nil {
		t.Fatalf("带体构造不应报错: %v", err)
	}
	// 分隔符
	i := bytes.IndexByte(out, 0)
	if i < 0 {
		t.Fatalf("带体输出必须含分隔符")
	}
	var header struct {
		BodySize int `json:"body_size"`
		Cmd      string `json:"cmd"`
	}
	if err := json.Unmarshal(out[:i], &header); err != nil {
		t.Fatalf("头应合法: %v", err)
	}
	if header.BodySize != 3 {
		t.Fatalf("body_size 应被实际体长覆盖为 3, got %d", header.BodySize)
	}
	if header.Cmd != "FileUploadData" {
		t.Fatalf("其余字段应保留, cmd=%s", header.Cmd)
	}
	if !bytes.Equal(out[i+1:], bin) {
		t.Fatalf("体应原样保留")
	}
}

func TestBuildFrame头非法拒绝(t *testing.T) {
	if _, err := BuildFrame([]byte("not json"), nil); err == nil {
		t.Fatalf("空体 + 非法头应报错")
	}
	if _, err := BuildFrame([]byte("not json"), []byte{1}); err == nil {
		t.Fatalf("带体 + 非法头应报错")
	}
	// 头是合法 JSON 但不是对象(数组): map 目标装不下, 拒绝
	if _, err := BuildFrame([]byte(`[1,2]`), []byte{1}); err == nil {
		t.Fatalf("非对象头应报错")
	}
}

func TestDecodeResponse纯JSON无binary(t *testing.T) {
	resp, err := decodeResponse([]byte(`{"status":"success","data":{"ok":1}}`))
	if err != nil {
		t.Fatalf("纯 JSON 响应不应报错: %v", err)
	}
	if _, has := resp["__binary__"]; has {
		t.Fatalf("纯 JSON 响应不应带 __binary__")
	}
}

func TestDecodeResponse混合帧挂binary(t *testing.T) {
	jpeg := []byte{0xff, 0xd8, 0xff, 0x00, 0xd9}
	raw := append([]byte(`{"status":"success","data":{"format":"jpeg"},"request_id":"r-1"}`), 0)
	raw = append(raw, jpeg...)

	resp, err := decodeResponse(raw)
	if err != nil {
		t.Fatalf("混合帧响应不应报错: %v", err)
	}
	got, ok := resp["__binary__"].([]byte)
	if !ok {
		t.Fatalf("__binary__ 应为 []byte")
	}
	if !bytes.Equal(got, jpeg) {
		t.Fatalf("__binary__ 内容应完整, got %v", got)
	}
	if resp["status"] != "success" {
		t.Fatalf("其余字段应保留")
	}
}
