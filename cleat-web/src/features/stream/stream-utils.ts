// 流媒体编解码工具(移植自 cleat-web MonitorPage):
// base64 → 字节 / ImageBitmap(Base64 JPEG 帧), PCM → AudioBuffer 播放, PCM → WAV 落盘。

/** base64 → 原始字节 */
export function base64ToBytes(b64: string): Uint8Array {
  const bin = atob(b64)
  const bytes = new Uint8Array(bin.length)
  for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i)
  return bytes
}

/** base64(JPEG) → ImageBitmap(桌面流/摄像头帧统一走它解码) */
export async function b64ToBitmap(b64: string): Promise<ImageBitmap> {
  const blob = new Blob([base64ToBytes(b64) as BlobPart], { type: 'image/jpeg' })
  return createImageBitmap(blob)
}

/** 时间戳 → HH:MM:SS(监控日志行用) */
export function fmtClock(ts: number): string {
  const d = new Date(ts)
  const pad = (n: number) => String(n).padStart(2, '0')
  return `${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`
}

/**
 * PCM 裸流 → AudioBuffer(边收边播)。
 * bits: 8/16/32; 小端; 交错声道。
 */
export function decodePcmToBuffer(
  ctx: AudioContext,
  bytes: Uint8Array,
  sampleRate: number,
  channels: number,
  bits: number
): AudioBuffer | null {
  const bytesPerSample = Math.max(1, bits / 8)
  const frameCount = Math.floor(bytes.length / (bytesPerSample * channels))
  if (frameCount <= 0) return null
  const buffer = ctx.createBuffer(channels, frameCount, sampleRate)
  for (let ch = 0; ch < channels; ch++) {
    const data = buffer.getChannelData(ch)
    for (let i = 0; i < frameCount; i++) {
      const offset = (i * channels + ch) * bytesPerSample
      let v = 0
      if (bits === 16) {
        v = ((bytes[offset] ?? 0) | ((bytes[offset + 1] ?? 0) << 8)) / 32768
      } else if (bits === 8) {
        v = ((bytes[offset] ?? 0) - 128) / 128
      } else if (bits === 32) {
        v = new DataView(bytes.buffer, bytes.byteOffset + offset, 4).getInt32(0, true) / 2147483648
      }
      data[i] = Math.max(-1, Math.min(1, v))
    }
  }
  return buffer
}

/** PCM → WAV 文件字节(44 字节标准头 + 数据) */
export function encodeWav(pcm: Uint8Array, sampleRate: number, channels: number, bits: number): Uint8Array {
  const blockAlign = (bits / 8) * channels
  const dataSize = pcm.length
  const buffer = new ArrayBuffer(44 + dataSize)
  const view = new DataView(buffer)
  const writeStr = (offset: number, s: string) => {
    for (let i = 0; i < s.length; i++) view.setUint8(offset + i, s.charCodeAt(i))
  }
  writeStr(0, 'RIFF')
  view.setUint32(4, 36 + dataSize, true)
  writeStr(8, 'WAVE')
  writeStr(12, 'fmt ')
  view.setUint32(16, 16, true)
  view.setUint16(20, 1, true) // PCM
  view.setUint16(22, channels, true)
  view.setUint32(24, sampleRate, true)
  view.setUint32(28, sampleRate * blockAlign, true)
  view.setUint16(32, blockAlign, true)
  view.setUint16(34, bits, true)
  writeStr(36, 'data')
  view.setUint32(40, dataSize, true)
  new Uint8Array(buffer, 44).set(pcm)
  return new Uint8Array(buffer)
}

/** 触发浏览器下载(任意字节 → 文件) */
export function downloadBytes(bytes: Uint8Array, filename: string, mime: string): void {
  const blob = new Blob([bytes as BlobPart], { type: mime })
  const url = URL.createObjectURL(blob)
  const a = document.createElement('a')
  a.href = url
  a.download = filename
  a.click()
  URL.revokeObjectURL(url)
}
