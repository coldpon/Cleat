// 文件管理器(真实对接 ): 协议对齐 cleat-web FileManagerPage ——
//   盘符 FileGetDrives / 列目录 FileListDir(单飞+代次, 防快速连点错目录) /
//   删除 FileDelete / 重命名 FileMove / 新建目录 FileCreateDir /
//   下载 FileDownloadReq → SSE file_download_data 分块(base64)拼装 → Blob 落盘 /
//   上传 FileUploadReq → upload-chunk(1MB 分块, 等 Agent 回执)。
// UI 壳保留新版设计(左目录树 + 右文件表 + 面包屑 + 状态栏); 平台感知: Linux/macOS 会话用 / 路径。
// 传输协议字段: FileEntry { name, size, is_directory, is_hidden, is_readonly, modified_time(ISO) }。
import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import { useQuery } from '@tanstack/react-query'
import {
  ArrowUp,
  Copy,
  Download,
  File as FileIcon,
  FileArchive,
  FileCode2,
  FileCog,
  FileText,
  Folder,
  FolderPlus,
  HardDrive,
  Image as ImageIcon,
  MoreHorizontal,
  PenLine,
  RefreshCw,
  Trash2,
  Upload,
} from 'lucide-react'
import { toast } from 'sonner'
import { connectSessionEvents, listSessions, sendCommand, uploadChunk, type CleatEvent, type CommandResponse } from '@/lib/cleat-api'
import { cn } from '@/lib/utils'
import { useVirtualRows } from '@/hooks/use-virtual-rows'
import {
  AlertDialog,
  AlertDialogAction,
  AlertDialogCancel,
  AlertDialogContent,
  AlertDialogDescription,
  AlertDialogFooter,
  AlertDialogHeader,
  AlertDialogTitle,
} from '@/components/ui/alert-dialog'
import { Button } from '@/components/ui/button'
import {
  Dialog,
  DialogContent,
  DialogDescription,
  DialogFooter,
  DialogHeader,
  DialogTitle,
} from '@/components/ui/dialog'
import {
  DropdownMenu,
  DropdownMenuContent,
  DropdownMenuItem,
  DropdownMenuSeparator,
  DropdownMenuTrigger,
} from '@/components/ui/dropdown-menu'
import { Input } from '@/components/ui/input'
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from '@/components/ui/table'

// ---------- 类型与协议 ----------

interface FileEntry {
  name: string
  size: number
  is_directory: boolean
  is_hidden: boolean
  is_readonly: boolean
  modified_time: string
}

const CHUNK_SIZE = 1024 * 1024 // 1MB, 与老前端一致
const ROW_H = 36 // 文件行恒定高度(h-9), 虚拟滚动依赖此值

/** 命令响应 → data 对象(sendCommand 已把 status=error 抛成异常, 这里兜底取 data) */
function respData(resp: CommandResponse): Record<string, unknown> {
  const data = resp?.data
  if (data && typeof data === 'object' && !Array.isArray(data)) return data as Record<string, unknown>
  return {}
}

// ---------- 路径工具(平台感知) ----------

function joinPath(dir: string, name: string, sep: string): string {
  if (!dir) return name
  return dir.endsWith(sep) ? dir + name : dir + sep + name
}

function parentPath(p: string, sep: string): string | null {
  if (sep === '/') {
    if (p === '/') return null
    const i = p.lastIndexOf('/')
    return i <= 0 ? '/' : p.slice(0, i)
  }
  if (/^[A-Za-z]:\\?$/.test(p)) return null
  const i = p.lastIndexOf('\\')
  if (i <= 2) return p.slice(0, 3)
  return p.slice(0, i)
}

function crumbsOf(p: string, sep: string): string[] {
  if (sep === '/') {
    const out = ['/']
    let cur = ''
    for (const seg of p.split('/').filter(Boolean)) {
      cur += '/' + seg
      out.push(cur)
    }
    return out
  }
  const out: string[] = []
  const drive = p.slice(0, 3)
  out.push(drive)
  const rest = p.slice(3).split('\\').filter(Boolean)
  let cur = drive
  for (const seg of rest) {
    cur = joinPath(cur, seg, '\\')
    out.push(cur)
  }
  return out
}

// ---------- 展示工具 ----------

function formatSize(n: number): string {
  if (!n || n <= 0) return '—'
  const k = 1024
  const sizes = ['B', 'KB', 'MB', 'GB', 'TB']
  const i = Math.min(sizes.length - 1, Math.floor(Math.log(n) / Math.log(k)))
  return `${parseFloat((n / Math.pow(k, i)).toFixed(1))} ${sizes[i]}`
}

function formatTime(t: string): string {
  if (!t) return '—'
  const m = t.match(/^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2})/)
  if (m) return `${m[1]}-${m[2]}-${m[3]} ${m[4]}:${m[5]}:${m[6]}`
  return t
}

function base64ToBytes(b64: string): Uint8Array {
  const bin = atob(b64)
  const bytes = new Uint8Array(bin.length)
  for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i)
  return bytes
}

function entryIcon(f: FileEntry) {
  if (f.is_directory) return <Folder className='size-4 fill-amber-400/70 text-amber-500' />
  const ext = f.name.split('.').pop()?.toLowerCase() || ''
  if (['exe', 'dll', 'sys'].includes(ext)) return <FileCog className='size-4 text-sky-500' />
  if (['zip', 'rar', '7z', 'gz'].includes(ext)) return <FileArchive className='size-4 text-violet-500' />
  if (['txt', 'log', 'md', 'ini', 'ovpn'].includes(ext)) return <FileText className='size-4 text-slate-400' />
  if (['ps1', 'py', 'sh', 'bat', 'cna', 'js'].includes(ext)) return <FileCode2 className='size-4 text-emerald-500' />
  if (['png', 'jpg', 'jpeg', 'gif', 'bmp'].includes(ext)) return <ImageIcon className='size-4 text-pink-500' />
  if (['docx', 'doc', 'xlsx', 'pptx', 'pdf'].includes(ext)) return <FileText className='size-4 text-blue-500' />
  return <FileIcon className='size-4 text-muted-foreground' />
}

// ---------- 目录树 ----------

function TreeNodes({
  path,
  depth,
  currentPath,
  sep,
  expanded,
  onToggle,
  onNavigate,
  childrenOf,
}: {
  path: string
  depth: number
  currentPath: string
  sep: string
  expanded: Set<string>
  onToggle: (p: string) => void
  onNavigate: (p: string) => void
  childrenOf: (p: string) => FileEntry[]
}) {
  // 未展开不渲染(懒加载: 展开时由 onToggle 触发 loadDirectory 填缓存)
  if (!expanded.has(path)) return null
  const children = childrenOf(path).filter((f) => f.is_directory)

  return (
    <>
      {children.map((c) => {
        const childPath = joinPath(path, c.name, sep)
        const isCurrent = childPath === currentPath
        const isOpen = expanded.has(childPath)
        return (
          <div key={childPath}>
            <button
              className={cn(
                'group flex w-full items-center gap-1 rounded-md py-[3px] pe-2 text-left text-[13px] transition-colors hover:bg-accent',
                isCurrent && 'bg-accent font-medium'
              )}
              style={{ paddingInlineStart: 8 + depth * 14 }}
              onClick={() => onNavigate(childPath)}
            >
              <span
                className='flex size-3.5 shrink-0 items-center justify-center'
                onClick={(e) => {
                  e.stopPropagation()
                  onToggle(childPath)
                }}
              >
                <svg
                  viewBox='0 0 24 24'
                  className={cn('size-3 text-muted-foreground transition-transform', isOpen && 'rotate-90')}
                >
                  <path d='M9 6l6 6-6 6' fill='none' stroke='currentColor' strokeWidth='2.4' strokeLinecap='round' />
                </svg>
              </span>
              <Folder className={cn('size-3.5 shrink-0', isOpen || isCurrent ? 'fill-amber-400/70 text-amber-500' : 'text-amber-500/80')} />
              <span className='truncate'>{c.name}</span>
            </button>
            {isOpen && (
              <TreeNodes
                path={childPath}
                depth={depth + 1}
                currentPath={currentPath}
                sep={sep}
                expanded={expanded}
                onToggle={onToggle}
                onNavigate={onNavigate}
                childrenOf={childrenOf}
              />
            )}
          </div>
        )
      })}
    </>
  )
}

// ---------- 主组件 ----------

export function FileManager({ sessionId }: { sessionId: number }) {
  // 会话 OS → 平台感知(路径分隔符/根): 复用 ['sessions'] 共享缓存, 不单独发请求
  const { data: sessData } = useQuery({
    queryKey: ['sessions'],
    queryFn: listSessions,
    refetchInterval: 3000,
  })
  const session = sessData?.sessions.find((s) => s.id === sessionId)
  const os = session?.os ?? ''
  const offline = !!sessData && !session?.online
  // 离线守卫(ref 保证回调里拿到最新值): 离线时所有命令直接跳过, 避免傻等 310s 命令超时。
  // ref 同步放 effect(渲染期写 ref 被 react-hooks/refs 禁止); 事件回调是异步的, 时序足够。
  const offlineRef = useRef(offline)
  useEffect(() => {
    offlineRef.current = offline
  }, [offline])
  const linux = /linux|ubuntu|kali|darwin|macos|amazon/i.test(os)
  const SEP = linux ? '/' : '\\'
  const ROOT = linux ? '/' : 'C:\\'

  const [path, setPath] = useState(ROOT)
  const [drives, setDrives] = useState<string[]>([])
  const [expanded, setExpanded] = useState<Set<string>>(() => new Set([ROOT]))
  const [selected, setSelected] = useState<string | null>(null)
  const [loading, setLoading] = useState(false)
  const [transferring, setTransferring] = useState(false)
  const [progressText, setProgressText] = useState('')
  const [newFolderOpen, setNewFolderOpen] = useState(false)
  const [newName, setNewName] = useState('')
  const [renameTarget, setRenameTarget] = useState<FileEntry | null>(null)
  const [delTarget, setDelTarget] = useState<FileEntry | null>(null)
  // 右键菜单: row=null 表示在空白处右键(针对当前目录)
  const [ctx, setCtx] = useState<{ x: number; y: number; row: FileEntry | null } | null>(null)

  // 目录缓存(树/表共用) + 版本号触发渲染; 单飞 + 代次(见 loadDirectory)
  const [dirVersion, setDirVersion] = useState(0)
  const dirCacheRef = useRef<Map<string, FileEntry[]>>(new Map())
  const genRef = useRef(0)
  const inFlightRef = useRef<Map<string, Promise<void>>>(new Map())

  // 下载上下文
  const dlRef = useRef<{
    name: string
    taskId: string
    chunks: Map<number, Uint8Array>
    total: number
    status: 'active' | 'done' | 'error'
  } | null>(null)
  const dlDisconnectRef = useRef<(() => void) | null>(null)
  const dlOpenRef = useRef<((err?: Error) => void) | null>(null)

  const childrenOf = useCallback(
    (p: string): FileEntry[] => dirCacheRef.current.get(p) ?? [],
    []
  )

  // ---- 盘符 ----
  const loadDrives = useCallback(async () => {
    if (offlineRef.current) return
    if (linux) {
      setDrives(['/'])
      return
    }
    try {
      const resp = await sendCommand(sessionId, 'FileGetDrives')
      const raw = respData(resp).drives
      if (Array.isArray(raw)) {
        const ds = raw
          .map((d) => {
            if (typeof d === 'string') return d
            const o = d as Record<string, unknown>
            return String(o.path || o.name || o.letter || '')
          })
          .filter(Boolean)
        setDrives(ds.length ? ds : ['C:\\'])
      }
    } catch (e) {
      toast.error(`获取盘符失败: ${(e as Error).message}`)
      setDrives(['C:\\'])
    }
  }, [sessionId, linux])

  // ---- 列目录(单飞 + 代次: 快速连点不串目录、同路径不重发) ----
  const loadDirectory = useCallback(
    async (p: string) => {
      if (!sessionId || !p || offlineRef.current) return
      const pending = inFlightRef.current.get(p)
      if (pending) return pending
      const gen = ++genRef.current
      const task = (async () => {
        setLoading(true)
        try {
          const resp = await sendCommand(sessionId, 'FileListDir', { path: p })
          // 期间用户又切了目录 → 本次结果作废, 不写缓存(避免旧目录内容闪一下)
          if (gen !== genRef.current) return
          const data = respData(resp)
          const list = ((data.entries as unknown[]) || []).map((f) => f as FileEntry)
          dirCacheRef.current.set(p, list)
          setDirVersion((v) => v + 1)
        } catch (e) {
          if (gen === genRef.current) {
            dirCacheRef.current.set(p, [])
            setDirVersion((v) => v + 1)
            toast.error(`列目录失败: ${(e as Error).message}`)
          }
        } finally {
          inFlightRef.current.delete(p)
          if (gen === genRef.current) setLoading(false)
        }
      })()
      inFlightRef.current.set(p, task)
      return task
    },
    [sessionId]
  )

  // OS/会话变化 → 重置根目录与盘符(首次挂载 os 未知, 默认 Windows; 检测到 Linux 再切换)。
  // 这是一次"重置状态 + 清缓存 + 重新拉盘符"的组合动作(非纯派生), react-hooks 的
  // set-state-in-effect 规则不适用, 定向豁免。
  /* eslint-disable react-hooks/set-state-in-effect -- 见上: 会话切换的组合式重置 */
  useEffect(() => {
    setPath(ROOT)
    setExpanded(new Set([ROOT]))
    setSelected(null)
    dirCacheRef.current.clear()
    setDirVersion((v) => v + 1)
    if (linux) setDrives(['/'])
    else void loadDrives()
  }, [ROOT, linux, sessionId, loadDrives])
  /* eslint-enable react-hooks/set-state-in-effect */

  // 路径变化 → 列目录
  useEffect(() => {
    if (!path) return
    void loadDirectory(path)
  }, [path, loadDirectory])

  // 卸载清理: 断开下载 SSE
  useEffect(() => {
    return () => {
      dlDisconnectRef.current?.()
      dlDisconnectRef.current = null
    }
  }, [])

  // ---- 导航 ----
  const navigate = (p: string) => {
    setPath(p)
    setSelected(null)
    setExpanded((prev) => {
      if (prev.has(p)) return prev
      const n = new Set(prev)
      n.add(p)
      return n
    })
    void loadDirectory(p)
  }

  const toggleExpand = (p: string) => {
    setExpanded((prev) => {
      const n = new Set(prev)
      if (n.has(p)) n.delete(p)
      else {
        n.add(p)
        void loadDirectory(p)
      }
      return n
    })
  }

  const goUp = () => {
    const up = parentPath(path, SEP)
    if (up) navigate(up)
  }

  // ---- 文件操作 ----
  const targetPath = (row: FileEntry) => joinPath(path, row.name, SEP)

  const nameValid = (n: string) => (linux ? /^[^/]+$/.test(n) : /^[^\\/:*?"<>|]+$/.test(n))

  const doDelete = async (row: FileEntry) => {
    try {
      await sendCommand(sessionId, 'FileDelete', { path: targetPath(row) })
      dirCacheRef.current.delete(targetPath(row))
      toast.success(`已删除 ${row.name}`)
      void loadDirectory(path)
    } catch (e) {
      toast.error(`删除失败: ${(e as Error).message}`)
    }
  }

  const doRename = async (row: FileEntry, newName: string) => {
    const n = newName.trim()
    if (!n || n === row.name) return
    if (!nameValid(n)) {
      toast.error(linux ? '非法文件名（不能包含 /）' : '非法文件名（不能包含 \\ / : * ? " < > |）')
      return
    }
    try {
      await sendCommand(sessionId, 'FileMove', { src: targetPath(row), dst: joinPath(path, n, SEP) })
      toast.success(`已重命名为 ${n}`)
      void loadDirectory(path)
    } catch (e) {
      toast.error(`重命名失败: ${(e as Error).message}`)
    }
  }

  const createFolder = async () => {
    const name = newName.trim()
    if (!name) return
    try {
      await sendCommand(sessionId, 'FileCreateDir', { path: joinPath(path, name, SEP) })
      setNewFolderOpen(false)
      setNewName('')
      toast.success(`已创建目录 ${name}`)
      void loadDirectory(path)
    } catch (e) {
      toast.error(`创建目录失败: ${(e as Error).message}`)
    }
  }

  const copyPath = (row: FileEntry) => {
    navigator.clipboard
      .writeText(targetPath(row))
      .then(() => toast.success('已复制路径'))
      .catch(() => toast.error('复制失败（浏览器拒绝访问剪贴板）'))
  }

  // ---- 下载(SSE 分块 → Blob 落盘) ----
  const closeDownloadSSE = () => {
    dlOpenRef.current = null
    dlDisconnectRef.current?.()
    dlDisconnectRef.current = null
  }

  const finalizeDownload = () => {
    const t = dlRef.current
    if (!t || t.status !== 'active') return
    const missing = t.total > 0 ? Math.max(0, t.total - t.chunks.size) : 1
    if (missing > 0 || t.chunks.size === 0) {
      t.status = 'error'
      setTransferring(false)
      setProgressText('')
      toast.error(`下载失败: ${t.name} 分块不完整（收到 ${t.chunks.size}/${t.total || '?'}）`)
      closeDownloadSSE()
      return
    }
    const ids = [...t.chunks.keys()].sort((a, b) => a - b)
    const parts: Uint8Array[] = []
    let size = 0
    for (const id of ids) {
      const b = t.chunks.get(id)
      if (b) {
        parts.push(b)
        size += b.length
      }
    }
    const blob = new Blob(parts as BlobPart[], { type: 'application/octet-stream' })
    const url = URL.createObjectURL(blob)
    const a = document.createElement('a')
    a.href = url
    a.download = t.name || 'download'
    a.click()
    URL.revokeObjectURL(url)
    t.status = 'done'
    setTransferring(false)
    setProgressText('')
    toast.success(`下载完成: ${t.name}（${formatSize(size)}）`)
    closeDownloadSSE()
  }

  const handleDownloadEvent = (ev: CleatEvent) => {
    const t = dlRef.current
    if (!t || t.status !== 'active') return
    const d = (ev.data || {}) as Record<string, unknown>
    const evId = String(d.task_id || '')
    if (evId && t.taskId && evId !== t.taskId) return

    if (ev.type === 'file_download_data') {
      const idx = Number(d.chunk_index)
      if (!Number.isFinite(idx) || idx < 0) return
      const total = Number(d.total_chunks) || 0
      if (total > 0) t.total = Math.max(t.total, total)
      const b64 = d.__binary__
      if (typeof b64 === 'string' && b64) t.chunks.set(idx, base64ToBytes(b64))
      if (t.total > 0) setProgressText(`下载中 ${t.name} ${Math.min(t.chunks.size, t.total)}/${t.total} 块`)
      if (t.total > 0 && t.chunks.size >= t.total) finalizeDownload()
    } else if (ev.type === 'file_download_complete') {
      finalizeDownload()
    } else if (ev.type === 'file_download_error') {
      t.status = 'error'
      setTransferring(false)
      setProgressText('')
      toast.error(`下载失败: ${typeof d.error === 'string' ? d.error : '未知错误'}`)
      closeDownloadSSE()
    }
  }

  const downloadRow = async (row: FileEntry) => {
    if (transferring) {
      toast.warning('已有传输任务进行中')
      return
    }
    if (row.is_directory) {
      toast.error('暂不支持下载目录，请进入目录选择文件')
      return
    }
    setTransferring(true)
    dlRef.current = { name: row.name, taskId: '', chunks: new Map(), total: 0, status: 'active' }
    setProgressText(`发起下载 ${row.name}…`)
    // 先建 SSE 订阅(等建流成功) 再发请求, 否则分块会漏
    const opened = new Promise<void>((resolve, reject) => {
      dlOpenRef.current = (err?: Error) => (err ? reject(err) : resolve())
    })
    const timer = window.setTimeout(() => dlOpenRef.current?.(new Error('SSE 连接超时')), 8000)
    dlDisconnectRef.current = connectSessionEvents(sessionId, handleDownloadEvent, () => dlOpenRef.current?.())
    try {
      await opened
      window.clearTimeout(timer)
      const resp = await sendCommand(sessionId, 'FileDownloadReq', { path: targetPath(row) })
      const taskId = String(respData(resp).task_id || '')
      if (!taskId) throw new Error('未返回 task_id')
      if (dlRef.current && dlRef.current.status === 'active') dlRef.current.taskId = taskId
    } catch (e) {
      if (dlRef.current) dlRef.current.status = 'error'
      setTransferring(false)
      setProgressText('')
      toast.error(`下载失败: ${(e as Error).message}`)
      closeDownloadSSE()
    }
  }

  // ---- 上传(1MB 分块) ----
  const uploadSelected = () => {
    if (transferring) {
      toast.warning('已有传输任务进行中')
      return
    }
    const input = document.createElement('input')
    input.type = 'file'
    input.onchange = async () => {
      const file = input.files?.[0]
      if (!file) return
      const target = joinPath(path, file.name, SEP)
      const totalChunks = Math.max(1, Math.ceil(file.size / CHUNK_SIZE))
      setTransferring(true)
      setProgressText(`发起上传 ${file.name}…`)
      try {
        const resp = await sendCommand(sessionId, 'FileUploadReq', {
          path: target,
          total_size: file.size,
          total_chunks: totalChunks,
        })
        const taskId = String(respData(resp).task_id || '')
        if (!taskId) throw new Error('未返回 task_id')
        for (let i = 0; i < totalChunks; i++) {
          const start = i * CHUNK_SIZE
          const end = Math.min(file.size, start + CHUNK_SIZE)
          const bytes = new Uint8Array(await file.slice(start, end).arrayBuffer())
          await uploadChunk(sessionId, taskId, i, bytes)
          setProgressText(`上传中 ${file.name} ${i + 1}/${totalChunks}`)
        }
        toast.success(`上传完成: ${file.name}（${formatSize(file.size)}）`)
        void loadDirectory(path)
      } catch (e) {
        toast.error(`上传失败: ${(e as Error).message}`)
      } finally {
        setTransferring(false)
        setProgressText('')
      }
    }
    input.click()
  }

  // ---- 列表数据 ----
  // 渲染要"当前目录列表": 缓存本体在 ref(异步回调读写), dirVersion 为失效信号。
  // 数据流是"写缓存必 bump 版本"的受控模式(撕裂不成立), 对 react-hooks/refs 定向豁免。
  // eslint-disable-next-line react-hooks/refs, react-hooks/exhaustive-deps
  const files = useMemo(() => dirCacheRef.current.get(path) ?? [], [path, dirVersion])
  const entries = useMemo(
    () =>
      [...files].sort((a, b) =>
        a.is_directory === b.is_directory ? a.name.localeCompare(b.name, undefined, { numeric: true }) : a.is_directory ? -1 : 1
      ),
    [files]
  )

  const crumbList = crumbsOf(path, SEP)
  const dirCount = entries.filter((f) => f.is_directory).length
  const fileCount = entries.length - dirCount
  // 大目录(C:\Windows\System32 级别 300+ 项)只渲染可见行(滚动容器 ref 由本组件持有, 传给钩子测算)
  const vRef = useRef<HTMLDivElement | null>(null)
  const v = useVirtualRows(entries.length, ROW_H, vRef)
  const visible = entries.slice(v.start, v.end)

  return (
    // select-none: 双击/拖选不再触发浏览器文本选择(Edge 复制浮层), 文件台是操作面板不是阅读区
    <div className='flex h-full min-h-0 select-none overflow-hidden rounded-lg border'>
      {/* 左: 目录树 */}
      <div className='flex w-60 shrink-0 flex-col border-e bg-muted/20'>
        <div className='flex h-9 shrink-0 items-center border-b px-3 text-xs font-medium text-muted-foreground'>
          目录
        </div>
        {offline && (
          <p className='border-b bg-amber-500/10 px-3 py-1.5 text-[11px] text-amber-600 dark:text-amber-400'>
            会话离线 —— 上线后自动恢复浏览
          </p>
        )}
        <div className='min-h-0 flex-1 overflow-y-auto py-1'>
          {drives.map((d) => (
            <div key={d}>
              <button
                className={cn(
                  'flex w-full items-center gap-1.5 rounded-md py-[3px] pe-2 text-left text-[13px] transition-colors hover:bg-accent',
                  path === d && 'bg-accent font-medium'
                )}
                style={{ paddingInlineStart: 10 }}
                onClick={() => navigate(d)}
              >
                <HardDrive className='size-3.5 shrink-0 text-muted-foreground' />
                <span>{d}</span>
              </button>
              <TreeNodes
                path={d}
                depth={1}
                currentPath={path}
                sep={SEP}
                expanded={expanded}
                onToggle={toggleExpand}
                onNavigate={navigate}
                childrenOf={childrenOf}
              />
            </div>
          ))}
          {drives.length === 0 && !offline && (
            <p className='px-3 py-2 text-xs text-muted-foreground'>{loading ? '加载盘符…' : '无可用盘符'}</p>
          )}
        </div>
      </div>

      {/* 右: 工具条 + 文件表 */}
      <div className='flex min-w-0 flex-1 flex-col'>
        <div className='flex h-9 shrink-0 items-center gap-1 border-b px-2'>
          <Button variant='ghost' size='icon' className='size-7' disabled={!parentPath(path, SEP)} onClick={goUp}>
            <ArrowUp className='size-4' />
          </Button>
          <Button variant='ghost' size='icon' className='size-7' onClick={() => void loadDirectory(path)}>
            <RefreshCw className={cn('size-3.5', loading && 'animate-spin')} />
          </Button>
          {/* 面包屑 */}
          <div className='mx-1 flex min-w-0 flex-1 items-center gap-0.5 overflow-x-auto text-[13px]'>
            {crumbList.map((c, i) => (
              <span key={c} className='flex shrink-0 items-center'>
                {i > 0 && <span className='px-0.5 text-muted-foreground/50'>›</span>}
                <button
                  className={cn(
                    'rounded px-1.5 py-0.5 transition-colors hover:bg-accent',
                    i === crumbList.length - 1 ? 'font-medium' : 'text-muted-foreground'
                  )}
                  onClick={() => navigate(c)}
                >
                  {SEP === '\\' && i === 0 ? c.replace('\\', '') : c.split(SEP).filter(Boolean).pop() || c}
                </button>
              </span>
            ))}
          </div>
          <Button variant='outline' size='sm' className='h-7 gap-1 text-xs' onClick={() => setNewFolderOpen(true)}>
            <FolderPlus className='size-3.5' />
            新建文件夹
          </Button>
          <Button variant='outline' size='sm' className='h-7 gap-1 text-xs' disabled={transferring} onClick={uploadSelected}>
            <Upload className='size-3.5' />
            上传
          </Button>
        </div>

        <div
          ref={vRef}
          className='min-h-0 flex-1 overflow-y-auto'
          onContextMenu={(e) => {
            e.preventDefault()
            setSelected(null)
            setCtx({ x: e.clientX, y: e.clientY, row: null })
          }}
        >
          <Table className='table-fixed'>
            <TableHeader className='sticky top-0 z-10 bg-background'>
              <TableRow className='hover:bg-transparent'>
                <TableHead className='h-8 text-xs'>名称</TableHead>
                <TableHead className='h-8 w-44 text-xs'>修改时间</TableHead>
                <TableHead className='h-8 w-24 text-xs'>大小</TableHead>
                <TableHead className='h-8 w-12' />
              </TableRow>
            </TableHeader>
            <TableBody>
              {v.padTop > 0 && (
                <TableRow aria-hidden className='hover:bg-transparent'>
                  <TableCell colSpan={4} className='border-0 p-0' style={{ height: v.padTop }} />
                </TableRow>
              )}
              {loading && entries.length === 0 ? (
                <TableRow>
                  <TableCell colSpan={4} className='h-32 text-center text-sm text-muted-foreground'>
                    加载中…
                  </TableCell>
                </TableRow>
              ) : entries.length === 0 ? (
                <TableRow>
                  <TableCell colSpan={4} className='h-32 text-center text-sm text-muted-foreground'>
                    此目录为空
                  </TableCell>
                </TableRow>
              ) : (
                visible.map((f) => (
                  <TableRow
                    key={f.name}
                    className={cn('h-9 cursor-default', selected === f.name && 'bg-accent', f.is_hidden && 'opacity-60')}
                    onClick={() => setSelected(f.name)}
                    onDoubleClick={() => f.is_directory && navigate(joinPath(path, f.name, SEP))}
                    onContextMenu={(e) => {
                      e.preventDefault()
                      e.stopPropagation()
                      setSelected(f.name)
                      setCtx({ x: e.clientX, y: e.clientY, row: f })
                    }}
                  >
                    <TableCell className='py-1'>
                      <span className='flex items-center gap-2 text-[13px]'>
                        {entryIcon(f)}
                        <span className='truncate'>{f.name}</span>
                        {f.is_readonly && <span className='shrink-0 text-[10px] text-muted-foreground'>只读</span>}
                      </span>
                    </TableCell>
                    <TableCell className='py-1 font-mono text-xs text-muted-foreground'>
                      {formatTime(f.modified_time)}
                    </TableCell>
                    <TableCell className='py-1 font-mono text-xs text-muted-foreground'>
                      {f.is_directory ? '—' : formatSize(f.size)}
                    </TableCell>
                    <TableCell className='py-1'>
                      <DropdownMenu>
                        <DropdownMenuTrigger asChild>
                          <Button
                            variant='ghost'
                            size='icon'
                            className='size-6'
                            onClick={(e) => e.stopPropagation()}
                          >
                            <MoreHorizontal className='size-3.5' />
                          </Button>
                        </DropdownMenuTrigger>
                        <DropdownMenuContent align='end' className='w-36'>
                          {f.is_directory && (
                            <DropdownMenuItem onClick={() => navigate(joinPath(path, f.name, SEP))}>
                              <Folder className='size-3.5' />
                              打开
                            </DropdownMenuItem>
                          )}
                          {!f.is_directory && (
                            <DropdownMenuItem onClick={() => void downloadRow(f)}>
                              <Download className='size-3.5' />
                              下载
                            </DropdownMenuItem>
                          )}
                          <DropdownMenuItem
                            onClick={() => {
                              setRenameTarget(f)
                              setNewName(f.name)
                            }}
                          >
                            <PenLine className='size-3.5' />
                            重命名
                          </DropdownMenuItem>
                          <DropdownMenuItem onClick={() => copyPath(f)}>
                            <Copy className='size-3.5' />
                            复制路径
                          </DropdownMenuItem>
                          <DropdownMenuSeparator />
                          <DropdownMenuItem variant='destructive' onClick={() => setDelTarget(f)}>
                            <Trash2 className='size-3.5' />
                            删除
                          </DropdownMenuItem>
                        </DropdownMenuContent>
                      </DropdownMenu>
                    </TableCell>
                  </TableRow>
                ))
              )}
              {v.padBottom > 0 && (
                <TableRow aria-hidden className='hover:bg-transparent'>
                  <TableCell colSpan={4} className='border-0 p-0' style={{ height: v.padBottom }} />
                </TableRow>
              )}
            </TableBody>
          </Table>
        </div>

        {/* 底部状态栏 */}
        <div className='flex h-7 shrink-0 items-center justify-between gap-3 border-t px-3 text-xs text-muted-foreground'>
          <span className='truncate font-mono'>{path}</span>
          <span className='shrink-0'>
            {transferring ? (
              <span className='text-foreground'>{progressText}</span>
            ) : (
              <>
                {dirCount} 个目录 · {fileCount} 个文件
                {selected ? ` · 已选 ${selected}` : ''}
              </>
            )}
          </span>
        </div>
      </div>

      {/* 右键菜单(行 = 针对文件/目录; 空白处 = 针对当前目录) */}
      {ctx && (
        <>
          <div
            className='fixed inset-0 z-50'
            onClick={() => setCtx(null)}
            onContextMenu={(e) => {
              e.preventDefault()
              setCtx(null)
            }}
          />
          <div
            className='fixed z-50 min-w-36 rounded-md border bg-popover p-1 text-popover-foreground shadow-md'
            style={{
              left: Math.min(ctx.x, window.innerWidth - 180),
              top: Math.min(ctx.y, window.innerHeight - (ctx.row ? 210 : 140)),
            }}
          >
            {ctx.row ? (
              <>
                {ctx.row.is_directory && (
                  <button
                    className='flex w-full items-center gap-2 rounded-sm px-2 py-1.5 text-left text-sm transition-colors hover:bg-accent hover:text-accent-foreground'
                    onClick={() => {
                      navigate(joinPath(path, ctx.row!.name, SEP))
                      setCtx(null)
                    }}
                  >
                    <Folder className='size-3.5 text-muted-foreground' />
                    打开
                  </button>
                )}
                {!ctx.row.is_directory && (
                  <button
                    className='flex w-full items-center gap-2 rounded-sm px-2 py-1.5 text-left text-sm transition-colors hover:bg-accent hover:text-accent-foreground'
                    onClick={() => {
                      void downloadRow(ctx.row!)
                      setCtx(null)
                    }}
                  >
                    <Download className='size-3.5 text-muted-foreground' />
                    下载
                  </button>
                )}
                <button
                  className='flex w-full items-center gap-2 rounded-sm px-2 py-1.5 text-left text-sm transition-colors hover:bg-accent hover:text-accent-foreground'
                  onClick={() => {
                    setRenameTarget(ctx.row!)
                    setNewName(ctx.row!.name)
                    setCtx(null)
                  }}
                >
                  <PenLine className='size-3.5 text-muted-foreground' />
                  重命名
                </button>
                <button
                  className='flex w-full items-center gap-2 rounded-sm px-2 py-1.5 text-left text-sm transition-colors hover:bg-accent hover:text-accent-foreground'
                  onClick={() => {
                    copyPath(ctx.row!)
                    setCtx(null)
                  }}
                >
                  <Copy className='size-3.5 text-muted-foreground' />
                  复制路径
                </button>
                <div className='my-1 border-t' />
                <button
                  className='flex w-full items-center gap-2 rounded-sm px-2 py-1.5 text-left text-sm text-red-500 transition-colors hover:bg-accent'
                  onClick={() => {
                    setDelTarget(ctx.row!)
                    setCtx(null)
                  }}
                >
                  <Trash2 className='size-3.5' />
                  删除
                </button>
              </>
            ) : (
              <>
                <button
                  className='flex w-full items-center gap-2 rounded-sm px-2 py-1.5 text-left text-sm transition-colors hover:bg-accent hover:text-accent-foreground'
                  onClick={() => {
                    void loadDirectory(path)
                    setCtx(null)
                  }}
                >
                  <RefreshCw className='size-3.5 text-muted-foreground' />
                  刷新
                </button>
                <button
                  className='flex w-full items-center gap-2 rounded-sm px-2 py-1.5 text-left text-sm transition-colors hover:bg-accent hover:text-accent-foreground'
                  onClick={() => {
                    setNewFolderOpen(true)
                    setCtx(null)
                  }}
                >
                  <FolderPlus className='size-3.5 text-muted-foreground' />
                  新建文件夹
                </button>
                <button
                  className='flex w-full items-center gap-2 rounded-sm px-2 py-1.5 text-left text-sm transition-colors hover:bg-accent hover:text-accent-foreground'
                  onClick={() => {
                    uploadSelected()
                    setCtx(null)
                  }}
                >
                  <Upload className='size-3.5 text-muted-foreground' />
                  上传到当前目录
                </button>
              </>
            )}
          </div>
        </>
      )}

      {/* 新建文件夹 */}
      <Dialog open={newFolderOpen} onOpenChange={setNewFolderOpen}>
        <DialogContent className='sm:max-w-sm'>
          <DialogHeader>
            <DialogTitle>新建文件夹</DialogTitle>
            <DialogDescription className='break-all font-mono text-xs'>{path}</DialogDescription>
          </DialogHeader>
          <Input
            value={newName}
            placeholder='文件夹名称'
            autoFocus
            onChange={(e) => setNewName(e.target.value)}
            onKeyDown={(e) => e.key === 'Enter' && void createFolder()}
          />
          <DialogFooter>
            <Button variant='outline' onClick={() => setNewFolderOpen(false)}>
              取消
            </Button>
            <Button onClick={() => void createFolder()}>创建</Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>

      {/* 重命名 */}
      <Dialog open={renameTarget !== null} onOpenChange={(o) => !o && setRenameTarget(null)}>
        <DialogContent className='sm:max-w-sm'>
          <DialogHeader>
            <DialogTitle>重命名「{renameTarget?.name}」</DialogTitle>
            <DialogDescription className='break-all font-mono text-xs'>{path}</DialogDescription>
          </DialogHeader>
          <Input
            value={newName}
            placeholder='新名称'
            autoFocus
            onChange={(e) => setNewName(e.target.value)}
            onKeyDown={(e) => {
              if (e.key === 'Enter' && renameTarget) {
                void doRename(renameTarget, newName)
                setRenameTarget(null)
              }
            }}
          />
          <DialogFooter>
            <Button variant='outline' onClick={() => setRenameTarget(null)}>
              取消
            </Button>
            <Button
              onClick={() => {
                if (renameTarget) void doRename(renameTarget, newName)
                setRenameTarget(null)
              }}
            >
              确认
            </Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>

      {/* 删除确认 */}
      <AlertDialog open={delTarget !== null} onOpenChange={(o) => !o && setDelTarget(null)}>
        <AlertDialogContent>
          <AlertDialogHeader>
            <AlertDialogTitle>删除「{delTarget?.name}」？</AlertDialogTitle>
            <AlertDialogDescription>
              {delTarget?.is_directory
                ? '目录及其内容会被一并删除，此操作不可恢复。'
                : '此操作在目标主机上不可恢复。'}
            </AlertDialogDescription>
          </AlertDialogHeader>
          <AlertDialogFooter>
            <AlertDialogCancel>取消</AlertDialogCancel>
            <AlertDialogAction
              onClick={() => {
                if (delTarget) void doDelete(delTarget)
                setDelTarget(null)
              }}
            >
              删除
            </AlertDialogAction>
          </AlertDialogFooter>
        </AlertDialogContent>
      </AlertDialog>
    </div>
  )
}
