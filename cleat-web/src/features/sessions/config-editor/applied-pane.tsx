// 提交结果面板(移植自 cleat-web configEditor/AppliedPane, 换 shadcn 壳):
// Agent 返回的 applied 摘要。两族回显语义(勿混淆):
// - heartbeat / reconnect / ... 等结构体域: applied[域] = 整域回显(当前生效全量)
// - runtime / http / persist(原散键域): applied[域] = 仅本次实际应用字段
//   —— "本次未改的字段没出现在回显里"不是失败, 是语义本身。
// - applied[域] 含 error 字符串(channels 校验拒绝等): 红色错误卡, 不阻止继续编辑。
// - 提交了但 applied 无该域: 灰色"未生效"。
import { Info } from 'lucide-react'
import { Badge } from '@/components/ui/badge'

/** 域级回显语义标注(仅本次应用 vs 整域回显) */
const PARTIAL_ECHO_DOMAINS = new Set(['runtime', 'http', 'persist'])

interface Props {
  applied: Record<string, unknown> | null
  /** 本次提交的顶层域(用于标出"未生效") */
  submittedDomains: string[]
}

function pretty(v: unknown): string {
  try {
    return JSON.stringify(v, null, 2) ?? String(v)
  } catch {
    return String(v)
  }
}

export function AppliedPane({ applied, submittedDomains }: Props) {
  if (!applied) {
    return <p className='py-10 text-center text-xs text-muted-foreground'>尚未提交</p>
  }

  const allDomains = Array.from(new Set([...submittedDomains, ...Object.keys(applied)])).filter(
    (d) => d !== '_startup' && d !== '_contract'
  )

  return (
    <div className='space-y-2 p-2'>
      <div className='flex gap-2 rounded-md border border-sky-500/30 bg-sky-500/5 p-2.5 text-xs leading-5'>
        <Info className='mt-0.5 size-3.5 shrink-0 text-sky-500' />
        <p className='text-muted-foreground'>
          runtime / http / persist 回显仅含本次实际应用字段；其余域为当前生效全量 —— 未改字段缺失不是失败
        </p>
      </div>
      {allDomains.length === 0 ? (
        <p className='py-10 text-center text-xs text-muted-foreground'>无域级回执</p>
      ) : (
        allDomains.map((domain) => {
          const inApplied = domain in applied
          const echo = applied[domain]
          const hasError =
            inApplied &&
            echo !== null &&
            typeof echo === 'object' &&
            !Array.isArray(echo) &&
            typeof (echo as Record<string, unknown>).error === 'string'

          let badgeCls = 'border-muted-foreground/30 text-muted-foreground'
          let badgeText = '未生效'
          let content: React.ReactNode = (
            <p className='text-xs text-muted-foreground'>Agent 回执未包含此域（可能未产生变化或被忽略）</p>
          )
          if (hasError) {
            badgeCls = 'border-red-500/40 bg-red-500/10 text-red-600 dark:text-red-400'
            badgeText = '被拒绝'
            content = (
              <div className='rounded-md border border-red-500/30 bg-red-500/5 p-2 text-xs'>
                <p className='mb-1 font-medium text-red-600 dark:text-red-400'>Agent 拒绝该域</p>
                <code className='break-all text-muted-foreground'>
                  {(echo as Record<string, unknown>).error as string}
                </code>
              </div>
            )
          } else if (inApplied) {
            badgeCls = 'border-emerald-500/40 bg-emerald-500/10 text-emerald-600 dark:text-emerald-400'
            badgeText = PARTIAL_ECHO_DOMAINS.has(domain) ? '已应用(本次字段)' : '已生效(整域回显)'
            content = (
              <pre className='m-0 max-h-64 overflow-auto whitespace-pre-wrap break-all rounded-md bg-muted/50 p-2.5 font-mono text-[11px] leading-5'>
                {pretty(echo)}
              </pre>
            )
          }

          return (
            <div key={domain} className='rounded-md border'>
              <div className='flex items-center gap-2 border-b px-2.5 py-1.5'>
                <Badge variant='outline' className={badgeCls}>
                  {badgeText}
                </Badge>
                <code className='font-mono text-xs'>{domain}</code>
              </div>
              <div className='p-2'>{content}</div>
            </div>
          )
        })
      )}
    </div>
  )
}
