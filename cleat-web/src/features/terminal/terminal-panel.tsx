// 真终端组件(xterm 全交互) —— 移植自 cleat-web TerminalPage 的核心数据流。
// 保留"真终端"能力: ANSI 渲染 / 流式输出 / 行式输入缓冲; 壳换为 Cleat 新版设计。
// 数据流: TerminalStart 启动 → terminal_output 事件(SSE) 实时写屏 → TerminalInput 发送。
// 增强(老页面没有): ↑/↓ 命令历史 / 启动后自动聚焦 / 连接成功提示。
import { useCallback, useEffect, useRef, useState } from 'react'
import { Terminal } from '@xterm/xterm'
import { FitAddon } from '@xterm/addon-fit'
import '@xterm/xterm/css/xterm.css'
import { connectSessionEvents, sendCommand } from '@/lib/cleat-api'
import { cn } from '@/lib/utils'
import { Button } from '@/components/ui/button'
import { Badge } from '@/components/ui/badge'
import { Eraser, RotateCw } from 'lucide-react'

type ShellStatus = 'starting' | 'active' | 'failed' | 'stopped'

const HISTORY_LIMIT = 100

export function TerminalPanel({ sessionId }: { sessionId: number }) {
  const hostRef = useRef<HTMLDivElement>(null)
  const termRef = useRef<Terminal | null>(null)
  const fitRef = useRef<FitAddon | null>(null)
  const lineBufRef = useRef('')
  const historyRef = useRef<string[]>([])
  const histIdxRef = useRef(-1)
  const [status, setStatus] = useState<ShellStatus>('starting')
  const [statusText, setStatusText] = useState('启动中…')

  // ---- 键盘输入(行式缓冲 + ↑/↓ 历史, Enter 提交) ----
  const handleData = useCallback(
    (data: string) => {
      const term = termRef.current
      if (!term) return
      const eraseLine = () => {
        // 用退格擦掉当前行缓冲(行式输入下光标恒在行尾)
        for (let k = 0; k < lineBufRef.current.length; k++) term.write('\b \b')
      }
      let i = 0
      while (i < data.length) {
        const ch = data[i]!
        if (ch === '\x1b' && data[i + 1] === '[') {
          // ESC [ 序列: 找终止字节; 仅处理 ↑/↓(命令历史), 其余跳过
          let j = i + 2
          while (j < data.length && !/[A-Za-z~]/.test(data[j]!)) j++
          const seq = data.slice(i, j + 1)
          i = j + 1
          if (seq.endsWith('A') || seq.endsWith('B')) {
            const hist = historyRef.current
            if (hist.length > 0) {
              if (seq.endsWith('A')) {
                histIdxRef.current = histIdxRef.current < 0 ? hist.length - 1 : Math.max(0, histIdxRef.current - 1)
              } else {
                histIdxRef.current = histIdxRef.current < 0 ? -1 : histIdxRef.current + 1
                if (histIdxRef.current >= hist.length) histIdxRef.current = -1
              }
              eraseLine()
              const next = histIdxRef.current >= 0 ? hist[histIdxRef.current]! : ''
              term.write(next)
              lineBufRef.current = next
            }
          }
          continue
        }
        if (ch === '\r') {
          const cmd = lineBufRef.current
          lineBufRef.current = ''
          term.write('\r\n')
          if (cmd.trim()) {
            const t = cmd.trim()
            if (historyRef.current[historyRef.current.length - 1] !== t) {
              historyRef.current.push(t)
              if (historyRef.current.length > HISTORY_LIMIT) historyRef.current.shift()
            }
            histIdxRef.current = -1
            void sendCommand(sessionId, 'TerminalInput', { command: cmd }).catch((e) => {
              term.write(`\r\n[发送失败: ${(e as Error).message}]\r\n`)
            })
          }
        } else if (ch === '\x7f' || ch === '\b') {
          if (lineBufRef.current.length > 0) {
            lineBufRef.current = lineBufRef.current.slice(0, -1)
            term.write('\b \b')
          }
        } else if (ch === '\x03') {
          term.write('^C\r\n')
          lineBufRef.current = ''
        } else if (ch >= ' ') {
          lineBufRef.current += ch
          term.write(ch)
        }
        i++
      }
    },
    [sessionId]
  )

  // ---- 生命周期: xterm 初始化 + SSE 订阅 + 启动 shell ----
  useEffect(() => {
    const host = hostRef.current
    if (!host) return

    const term = new Terminal({
      fontSize: 13,
      fontFamily: "'JetBrains Mono', 'Cascadia Code', Consolas, monospace",
      lineHeight: 1.35,
      cursorBlink: true,
      convertEol: true,
      scrollback: 5000,
      theme: {
        background: '#09090b',
        foreground: '#e4e4e7',
        cursor: '#38bdf8',
        cursorAccent: '#09090b',
        selectionBackground: '#1e5a8a88',
        brightGreen: '#3ddc84',
        brightRed: '#f2555a',
        brightYellow: '#f0b429',
      },
    })
    const fit = new FitAddon()
    term.loadAddon(fit)
    term.open(host)
    term.onData(handleData)
    try {
      fit.fit()
    } catch {
      /* 容器未就绪 */
    }
    const ro = new ResizeObserver(() => {
      try {
        fit.fit()
      } catch {
        /* ignore */
      }
    })
    ro.observe(host)
    // 标签页 keep-alive 隐藏/显示 → 重新可见时自动聚焦(切回终端不用再点一下)
    const io = new IntersectionObserver((entries) => {
      for (const e of entries) {
        if (e.isIntersecting) term.focus()
      }
    })
    io.observe(host)
    termRef.current = term
    fitRef.current = fit

    // 会话级事件流: terminal_output 实时写屏
    const disconnect = connectSessionEvents(sessionId, (ev) => {
      if (ev.type !== 'terminal_output') return
      const out = (ev.data as { output?: string } | undefined)?.output
      if (typeof out === 'string') term.write(out)
    })

    // 启动远程 shell
    ;(async () => {
      try {
        const resp = await sendCommand(sessionId, 'TerminalStart')
        if (resp.status === 'success') {
          setStatus('active')
          setStatusText('Shell 已连接')
          term.write('\x1b[90m[Shell session started]\x1b[0m\r\n')
          term.focus()
        } else {
          setStatus('failed')
          setStatusText(resp.message || '启动失败')
          term.write(`\r\n[启动失败: ${resp.message || 'unknown'}]\r\n`)
        }
      } catch (e) {
        setStatus('failed')
        setStatusText((e as Error).message)
        term.write(`\r\n[启动失败: ${(e as Error).message}]\r\n`)
      }
    })()

    return () => {
      void sendCommand(sessionId, 'TerminalClose').catch(() => {})
      disconnect()
      io.disconnect()
      ro.disconnect()
      term.dispose()
      termRef.current = null
      fitRef.current = null
    }
  }, [sessionId, handleData])

  const restart = async () => {
    const term = termRef.current
    term?.clear()
    setStatus('starting')
    setStatusText('重启中…')
    try {
      await sendCommand(sessionId, 'TerminalClose').catch(() => {})
      const resp = await sendCommand(sessionId, 'TerminalStart')
      if (resp.status === 'success') {
        setStatus('active')
        setStatusText('Shell 已连接')
      } else {
        setStatus('failed')
        setStatusText(resp.message || '启动失败')
      }
    } catch (e) {
      setStatus('failed')
      setStatusText((e as Error).message)
    }
  }

  return (
    <div className='flex h-full min-h-0 flex-col overflow-hidden rounded-lg border bg-[#09090b]'>
      <div className='flex flex-shrink-0 items-center justify-between border-b border-border/50 bg-card px-3 py-1.5'>
        <div className='flex items-center gap-2'>
          <span
            className={cn(
              'inline-block size-2 rounded-full transition-all',
              status === 'active'
                ? 'bg-emerald-500 shadow-[0_0_6px_rgba(16,185,129,0.5)]'
                : status === 'failed'
                  ? 'bg-red-500'
                  : 'bg-muted-foreground/40'
            )}
          />
          <span className='text-xs text-muted-foreground'>{statusText}</span>
          {status !== 'active' && (
            <Badge variant='outline' className='rounded-sm text-[10px]'>
              未连接
            </Badge>
          )}
        </div>
        <div className='flex items-center gap-1'>
          <Button size='sm' variant='ghost' className='h-7 text-xs' onClick={() => void restart()}>
            <RotateCw className='size-3.5' />
            重启
          </Button>
          <Button
            size='sm'
            variant='ghost'
            className='h-7 text-xs'
            onClick={() => termRef.current?.clear()}
          >
            <Eraser className='size-3.5' />
            清空
          </Button>
        </div>
      </div>
      <div ref={hostRef} className='min-h-0 flex-1 p-1' />
    </div>
  )
}
