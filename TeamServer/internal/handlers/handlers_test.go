package handlers

import (
	"testing"

	"cleat/teamserver/internal/eventbus"
	"cleat/teamserver/internal/session"
	"cleat/teamserver/internal/store"
)

// num 把 int/float64 数值统一为 int(单测里直传 int, 真实 JSON 传 float64)。
func num(v interface{}) int {
	switch n := v.(type) {
	case float64:
		return int(n)
	case int:
		return n
	case int64:
		return int(n)
	}
	return -1
}

// TestStreamForwarder 验证流媒体混合帧转发: 二进制体放到 image 字段 + 元数据透传。
func TestStreamForwarder(t *testing.T) {
	bus := eventbus.New()
	sub := bus.SubscribeSession(5)
	defer bus.Unsubscribe(5, sub)

	sess := &session.Session{Record: &store.Record{ID: 5}}
	fn := makeStreamForwarder(eventbus.EventDesktopStream, "image", bus)

	params := map[string]interface{}{
		"type": "desktop_full", "format": "jpeg", "width": 1920, "height": 1080,
	}
	fn(sess, "DesktopStreamData", params, []byte("JPEGBYTES"))

	select {
	case ev := <-sub:
		data := ev.Data.(map[string]interface{})
		img, ok := data["image"].([]byte)
		if !ok || len(img) == 0 {
			t.Fatalf("流帧事件应含 []byte image(bytes->JSON 自动 base64), got %#v", data["image"])
		}
		if string(img) != "JPEGBYTES" {
			t.Fatalf("image 应为原始字节, got %q", img)
		}
		if data["type"] != "desktop_full" || num(data["width"]) != 1920 {
			t.Fatalf("元数据应透传, got %v", data)
		}
		if num(data["size"]) != len("JPEGBYTES") {
			t.Fatalf("size 字段应为字节数, got %v", data["size"])
		}
	default:
		t.Fatal("应收到流帧事件")
	}
}

// TestStreamForwarderNoBinary 验证无二进制时不发事件(或为空 image)。
func TestStreamForwarderNoBinary(t *testing.T) {
	bus := eventbus.New()
	sub := bus.SubscribeSession(6)
	defer bus.Unsubscribe(6, sub)
	sess := &session.Session{Record: &store.Record{ID: 6}}
	fn := makeStreamForwarder(eventbus.EventAudioData, "audio", bus)
	fn(sess, "AudioData", map[string]interface{}{"type": "audio"}, nil)
	// 空二进制: 该帧无有效载荷, 按旧 Server 不应发布
	select {
	case ev := <-sub:
		t.Fatalf("无二进制流帧不应发布事件, got %v", ev)
	default:
	}
}
