// 变更预览面板(移植自 cleat-web configEditor/DiffPane, 换 shadcn 壳):
// diffConfig 产出的条目列表; channels 条目带"整链替换"警示 —— Agent 侧原子整链覆盖,
// 发什么就是什么, 未在数组中的通道会被移除。
import { TriangleAlert } from 'lucide-react'
import { Badge } from '@/components/ui/badge'
import type { DiffEntry } from './pseudo-json'

const TYPE_META: Record<DiffEntry['type'], { label: string; className: string }> = {
  changed: { label: '修改', className: 'border-sky-500/40 bg-sky-500/10 text-sky-600 dark:text-sky-400' },
  added: { label: '新增', className: 'border-emerald-500/40 bg-emerald-500/10 text-emerald-600 dark:text-emerald-400' },
  removed: { label: '删除', className: 'border-red-500/40 bg-red-500/10 text-red-600 dark:text-red-400' },
}

export function DiffPane({ entries }: { entries: DiffEntry[] }) {
  if (entries.length === 0) {
    return <p className='py-10 text-center text-xs text-muted-foreground'>与回读配置一致，无变更</p>
  }
  const touchesChannels = entries.some((e) => e.path.startsWith('channels'))
  return (
    <div className='space-y-2 p-2'>
      {touchesChannels && (
        <div className='flex gap-2 rounded-md border border-amber-500/40 bg-amber-500/10 p-2.5 text-xs leading-5'>
          <TriangleAlert className='mt-0.5 size-3.5 shrink-0 text-amber-500' />
          <div>
            <p className='font-medium text-amber-600 dark:text-amber-400'>channels 为整链替换（原子）</p>
            <p className='text-muted-foreground'>
              提交后该数组将原样覆盖 Agent 当前通道链，数组顺序即优先级 —— 未在数组中的通道会被移除，覆盖与否由操作员负责。
            </p>
          </div>
        </div>
      )}
      {entries.map((e, i) => (
        <div key={`${e.path}-${i}`} className='rounded-md border px-2.5 py-1.5 text-xs leading-5'>
          <div className='flex items-start gap-1.5'>
            <Badge variant='outline' className={TYPE_META[e.type].className}>
              {TYPE_META[e.type].label}
            </Badge>
            <code className='break-all font-mono text-[11px] text-muted-foreground'>{e.path}</code>
          </div>
          {e.type === 'changed' && (
            <div className='mt-1 break-all font-mono text-[11px]'>
              <span className='text-red-500 line-through decoration-red-300'>{e.oldValue}</span>
              <span className='mx-1.5 text-muted-foreground'>→</span>
              <span className='text-emerald-600 dark:text-emerald-400'>{e.newValue}</span>
            </div>
          )}
          {e.type === 'added' && (
            <div className='mt-1 break-all font-mono text-[11px] text-emerald-600 dark:text-emerald-400'>{e.newValue}</div>
          )}
          {e.type === 'removed' && <div className='mt-1 break-all font-mono text-[11px] text-red-500'>{e.oldValue}</div>}
        </div>
      ))}
    </div>
  )
}
