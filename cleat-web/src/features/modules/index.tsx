// 模块仓库(真实对接 ): REST /api/modules(列表/上传/删除)。
// 模块 = Agent 可动态加载的命令 DLL(mod_screenshot 等); 上传后可经 loadModule 装载进会话。
import { useState } from 'react'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { Blocks, RefreshCw, Trash2, Upload } from 'lucide-react'
import { toast } from 'sonner'
import { deleteModule, listModules, uploadModule } from '@/lib/cleat-rest'
import { cn } from '@/lib/utils'
import { Button } from '@/components/ui/button'
import {
  Dialog,
  DialogContent,
  DialogDescription,
  DialogFooter,
  DialogHeader,
  DialogTitle,
} from '@/components/ui/dialog'
import { Input } from '@/components/ui/input'
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from '@/components/ui/table'
import { Header } from '@/components/layout/header'
import { Main } from '@/components/layout/main'
import { ThemeSwitch } from '@/components/theme-switch'
import { ProfileDropdown } from '@/components/profile-dropdown'
import { Search } from '@/components/search'

function fmtSize(n: number): string {
  if (!n || n <= 0) return '—'
  const k = 1024
  const sizes = ['B', 'KB', 'MB']
  const i = Math.min(sizes.length - 1, Math.floor(Math.log(n) / Math.log(k)))
  return `${(n / Math.pow(k, i)).toFixed(1)} ${sizes[i]}`
}

export function Modules() {
  const qc = useQueryClient()
  const [uploadOpen, setUploadOpen] = useState(false)
  const [modName, setModName] = useState('')
  const [modFile, setModFile] = useState<File | null>(null)

  const q = useQuery({ queryKey: ['modules'], queryFn: listModules })
  const list = q.data?.modules ?? []

  const invalidate = () => void qc.invalidateQueries({ queryKey: ['modules'] })

  const upload = useMutation({
    mutationFn: () => uploadModule(modName.trim(), modFile!),
    onSuccess: (r) => {
      toast.success(`模块已上传：${r.name}（${fmtSize(r.size)}）`)
      setUploadOpen(false)
      setModName('')
      setModFile(null)
      invalidate()
    },
    onError: (e) => toast.error(`上传失败: ${(e as Error).message}`),
  })

  const del = useMutation({
    mutationFn: deleteModule,
    onSuccess: () => {
      toast.success('模块已删除')
      invalidate()
    },
    onError: (e) => toast.error(`删除失败: ${(e as Error).message}`),
  })

  const pickFile = () => {
    const input = document.createElement('input')
    input.type = 'file'
    input.onchange = () => {
      const f = input.files?.[0]
      if (f) {
        setModFile(f)
        if (!modName.trim()) setModName(f.name.replace(/\.(dll|so)$/i, ''))
      }
    }
    input.click()
  }

  const nameValid = /^[A-Za-z0-9_-]+$/.test(modName.trim())

  return (
    <>
      <Header fixed>
        <Search className='me-auto' />
        <ThemeSwitch />
        <ProfileDropdown />
      </Header>
      <Main>
        <div className='mb-4 flex flex-wrap items-end justify-between gap-2'>
          <div>
            <h2 className='flex items-center gap-2 text-2xl font-bold tracking-tight'>
              <Blocks className='size-5' />
              模块
            </h2>
            <p className='text-muted-foreground'>Agent 可加载的命令模块仓库 · 共 {list.length} 个</p>
          </div>
          <div className='flex gap-2'>
            <Button size='sm' variant='outline' onClick={() => void q.refetch()}>
              <RefreshCw className={cn('size-4', q.isFetching && 'animate-spin')} />
              刷新
            </Button>
            <Button size='sm' onClick={() => setUploadOpen(true)}>
              <Upload className='size-4' />
              上传模块
            </Button>
          </div>
        </div>

        <div className='overflow-hidden rounded-md border'>
          <Table>
            <TableHeader>
              <TableRow>
                <TableHead className='w-48'>模块 ID</TableHead>
                <TableHead className='w-24'>大小</TableHead>
                <TableHead>SHA256</TableHead>
                <TableHead className='w-44'>修改时间</TableHead>
                <TableHead className='w-16 text-end'>操作</TableHead>
              </TableRow>
            </TableHeader>
            <TableBody>
              {q.isLoading ? (
                <TableRow>
                  <TableCell colSpan={5} className='h-24 text-center text-sm text-muted-foreground'>
                    加载中…
                  </TableCell>
                </TableRow>
              ) : q.isError ? (
                <TableRow>
                  <TableCell colSpan={5} className='h-24 text-center text-sm text-muted-foreground'>
                    加载失败：{(q.error as Error).message}
                    <Button size='sm' variant='outline' className='ms-3 h-7 text-xs' onClick={() => void q.refetch()}>
                      重试
                    </Button>
                  </TableCell>
                </TableRow>
              ) : list.length === 0 ? (
                <TableRow>
                  <TableCell colSpan={5} className='h-24 text-center text-sm text-muted-foreground'>
                    模块仓库为空 · 上传模块 DLL 后可在会话中 loadModule 装载
                  </TableCell>
                </TableRow>
              ) : (
                list.map((m) => (
                  <TableRow key={m.name} className='h-9'>
                    <TableCell className='font-mono text-sm'>{m.name}</TableCell>
                    <TableCell className='font-mono text-xs text-muted-foreground'>{fmtSize(m.size)}</TableCell>
                    <TableCell className='truncate font-mono text-xs text-muted-foreground' title={m.sha256}>
                      {m.sha256 ? `${m.sha256.slice(0, 16)}…` : '—'}
                    </TableCell>
                    <TableCell className='font-mono text-xs text-muted-foreground'>
                      {m.modified ? m.modified.replace('T', ' ').slice(0, 19) : '—'}
                    </TableCell>
                    <TableCell className='text-end'>
                      <Button
                        size='sm'
                        variant='ghost'
                        className='h-6 w-6 p-0 text-muted-foreground hover:text-red-400'
                        title='删除'
                        onClick={() => {
                          if (window.confirm(`删除模块 ${m.name}？`)) del.mutate(m.name)
                        }}
                      >
                        <Trash2 className='size-3.5' />
                      </Button>
                    </TableCell>
                  </TableRow>
                ))
              )}
            </TableBody>
          </Table>
        </div>
      </Main>

      {/* 上传模块 */}
      <Dialog open={uploadOpen} onOpenChange={setUploadOpen}>
        <DialogContent className='sm:max-w-sm'>
          <DialogHeader>
            <DialogTitle>上传模块</DialogTitle>
            <DialogDescription>模块 ID 仅限字母/数字/下划线/连字符; 上传后可 loadModule 进会话</DialogDescription>
          </DialogHeader>
          <div className='grid gap-3'>
            <Input
              placeholder='模块 ID * (如 mod_screenshot)'
              value={modName}
              onChange={(e) => setModName(e.target.value)}
            />
            <Button variant='outline' className='justify-start text-muted-foreground' onClick={pickFile}>
              {modFile ? modFile.name : '选择文件（DLL/SO）…'}
            </Button>
          </div>
          <DialogFooter>
            <Button variant='outline' onClick={() => setUploadOpen(false)}>
              取消
            </Button>
            <Button disabled={!nameValid || !modFile || upload.isPending} onClick={() => upload.mutate()}>
              上传
            </Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>
    </>
  )
}
