// 只读信息面板(移植自 cleat-web configEditor/StartupPane, 换 shadcn 壳):
// GetConfig 回读里的 _startup 区(编译/启动期固定, SetConfig 不可改)。
// 前端不渲染、不提交该区 —— 仅在此只读展示, 避免"配置不全"误判。
import { Info } from 'lucide-react'

export function StartupPane({ startup }: { startup: Record<string, unknown> | null }) {
  if (!startup || Object.keys(startup).length === 0) {
    return <p className='py-10 text-center text-xs text-muted-foreground'>回读数据中无 _startup 区</p>
  }
  return (
    <div className='space-y-2 p-2'>
      <div className='flex gap-2 rounded-md border border-sky-500/30 bg-sky-500/5 p-2.5 text-xs leading-5'>
        <Info className='mt-0.5 size-3.5 shrink-0 text-sky-500' />
        <p className='text-muted-foreground'>编译/启动期固定参数，SetConfig 不可修改（如需变更需重新生成载荷）</p>
      </div>
      <div className='overflow-hidden rounded-md border'>
        {Object.entries(startup).map(([k, v], i) => (
          <div key={k} className={`flex items-start justify-between gap-3 px-2.5 py-1.5 text-xs ${i > 0 ? 'border-t' : ''}`}>
            <code className='shrink-0 font-mono text-muted-foreground'>{k}</code>
            <code className='break-all text-right font-mono'>{typeof v === 'object' ? JSON.stringify(v) : String(v)}</code>
          </div>
        ))}
      </div>
    </div>
  )
}
