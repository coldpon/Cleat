// 运行时配置面板(真实对接): 移植 cleat-web AgentConfigPage —— 契约驱动伪 JSON 编辑器。
// 数据流: GetConfig(DumpJson) → buildPseudoJson(带契约注释) → CodeMirror(JSON5) 编辑
//        → parsePseudoJson(解析+契约/域内校验) → sanitizeForSubmit(剔只读键) → SetConfig → applied 回执。
// 契约要点(与 Agent ConfigContract.hpp 对齐):
// - 顶层子域(现 16 域), 无散键; _startup/_contract/recovery.key 不渲染不提交;
// - 域顺序与注释来自回读 _contract(单一数据源), 本地兜底表仅应急;
// - channels 整链替换(原子): 非空链永远全量重发; 回读空链(未下发状态)且未编辑时提交剔除该键;
// - applied 两族回显: runtime/http/persist 仅本次应用字段, 其余域整域回显。
import { useCallback, useEffect, useMemo, useState } from 'react'
import { AlignLeft, CheckCircle2, RefreshCw, Send, TriangleAlert } from 'lucide-react'
import { toast } from 'sonner'
import { sendCommand } from '@/lib/cleat-api'
import { useCanWrite } from '@/hooks/use-can-write'
import { cn } from '@/lib/utils'
import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import {
  Dialog,
  DialogContent,
  DialogDescription,
  DialogFooter,
  DialogHeader,
  DialogTitle,
} from '@/components/ui/dialog'
import { Tabs, TabsList, TabsTrigger } from '@/components/ui/tabs'
import { ConfirmDialog } from '@/components/confirm-dialog'
import { FALLBACK_CONTRACT, type ContractKeySpec } from '@/lib/agent-config-schema'
import { AppliedPane } from './config-editor/applied-pane'
import { DiffPane } from './config-editor/diff-pane'
import { PseudoJsonEditor } from './config-editor/pseudo-json-editor'
import { StartupPane } from './config-editor/startup-pane'
import {
  buildPseudoJson,
  diffConfig,
  parsePseudoJson,
  sanitizeForSubmit,
  stripReadonly,
  type JsonObject,
} from './config-editor/pseudo-json'

type SideTab = 'diff' | 'applied' | 'startup'

export function ConfigPanel({ sessionId }: { sessionId: number }) {
  // 写操作门控: 提交配置(SetConfig)为写操作, 仅 admin/operator; 回读/格式化/校验为读操作。
  const canWrite = useCanWrite()

  const [text, setText] = useState('')
  const [loadedConfig, setLoadedConfig] = useState<JsonObject | null>(null)
  const [loading, setLoading] = useState(false)
  const [submitting, setSubmitting] = useState(false)
  const [applied, setApplied] = useState<Record<string, unknown> | null>(null)
  const [submittedDomains, setSubmittedDomains] = useState<string[]>([])
  const [sideTab, setSideTab] = useState<SideTab>('diff')
  const [refreshOpen, setRefreshOpen] = useState(false)
  const [issuesOpen, setIssuesOpen] = useState(false)

  // ---- 派生: 契约 / 只读区 ----
  const contract = useMemo<ContractKeySpec[]>(() => {
    const c = loadedConfig?._contract
    return Array.isArray(c) ? (c as ContractKeySpec[]) : FALLBACK_CONTRACT
  }, [loadedConfig])

  const startup = useMemo(
    () =>
      loadedConfig?._startup && typeof loadedConfig._startup === 'object'
        ? (loadedConfig._startup as Record<string, unknown>)
        : null,
    [loadedConfig]
  )

  // ---- 派生: 解析与校验(编辑即跑; 200 行量级 JSON5.parse 开销可忽略) ----
  const parsed = useMemo(() => (text.trim() ? parsePseudoJson(text, contract) : null), [text, contract])
  const errors = useMemo(() => parsed?.issues.filter((i) => i.severity === 'error') ?? [], [parsed])
  const warnings = useMemo(() => parsed?.issues.filter((i) => i.severity === 'warning') ?? [], [parsed])

  // ---- 派生: 变更预览(语法错误时无法对比, 列表为空 + 提示) ----
  const diffEntries = useMemo(() => {
    if (!loadedConfig || !parsed?.ok || !parsed.data) return []
    return diffConfig(stripReadonly(loadedConfig), stripReadonly(parsed.data))
  }, [loadedConfig, parsed])

  // ---- 回读 ----
  const loadConfig = useCallback(async () => {
    if (!sessionId || sessionId <= 0) return
    setLoading(true)
    setApplied(null)
    try {
      const resp = await sendCommand<{ config?: JsonObject }>(sessionId, 'GetConfig')
      const cfg = resp.data?.config
      if (resp.status === 'success' && cfg) {
        setLoadedConfig(cfg)
        setText(buildPseudoJson(cfg, Array.isArray(cfg._contract) ? (cfg._contract as ContractKeySpec[]) : undefined))
        toast.success('配置已回读')
      } else {
        toast.warning('回读失败或配置为空')
      }
    } catch (e) {
      toast.error(`回读失败: ${(e as Error).message}`)
    } finally {
      setLoading(false)
    }
  }, [sessionId])

  useEffect(() => {
    // 挂载即回读 GetConfig(async 拉取, 状态更新随 promise 落地)。
    // react-hooks/set-state-in-effect 对"异步数据拉取"误伤, 此处定向豁免。
    // eslint-disable-next-line react-hooks/set-state-in-effect
    void loadConfig()
  }, [loadConfig])

  // ---- 刷新(有未提交修改时确认覆盖) ----
  const refresh = () => {
    if (loadedConfig && diffEntries.length > 0) {
      setRefreshOpen(true)
    } else {
      void loadConfig()
    }
  }

  // ---- 格式化: 按契约顺序 + 域内字段序重新序列化(确定性输出) ----
  const format = () => {
    if (!parsed || parsed.parseError) {
      toast.error('存在语法错误，无法格式化（先修复编辑器中标红的语法问题）')
      return
    }
    setText(buildPseudoJson(parsed.data!, contract))
    toast.success('已按契约顺序格式化（注释重新生成）')
  }

  // ---- 校验: 全量跑一遍并弹结果 ----
  const validate = () => {
    if (!parsed) return
    if (parsed.parseError) {
      toast.error(`语法错误（第 ${parsed.parseError.line} 行第 ${parsed.parseError.column} 列）：${parsed.parseError.message}`)
      return
    }
    if (errors.length > 0) {
      setIssuesOpen(true)
    } else if (warnings.length > 0) {
      toast.warning(`校验通过，${warnings.length} 项提示（超范围将被 Agent 钳位 / 未知键将被忽略）`)
    } else {
      toast.success('校验通过')
    }
  }

  // ---- 提交: 解析 → 净化 → SetConfig → applied 展示 ----
  const submit = async () => {
    if (!canWrite) return
    if (!sessionId || !parsed) return
    if (parsed.parseError) {
      toast.error('存在语法错误，无法提交')
      return
    }
    if (!parsed.ok || !parsed.data) {
      validate()
      return
    }
    const { payload, removedKeys } = sanitizeForSubmit(parsed.data, loadedConfig)
    if (Object.keys(payload).length === 0) {
      toast.warning('没有可下发的配置域（全部为只读键或空链）')
      return
    }
    setSubmitting(true)
    try {
      const resp = await sendCommand<{ applied?: Record<string, unknown> }>(sessionId, 'SetConfig', payload)
      if (resp.status === 'success' && resp.data?.applied) {
        setApplied(resp.data.applied)
        setSubmittedDomains(Object.keys(payload))
        setSideTab('applied')
        toast.success(
          removedKeys.length > 0 ? `已下发（剔除只读键：${removedKeys.join('、')}）` : '配置已下发，见右侧回执'
        )
      } else {
        toast.warning(resp.message || '配置已下发（无 applied 摘要）')
      }
    } catch (e) {
      // 网络错误 / Agent 报错: 保留编辑器内容, 不阻止继续编辑
      toast.error(`下发失败: ${(e as Error).message}`)
    } finally {
      setSubmitting(false)
    }
  }

  return (
    <div className='flex h-full min-h-0 flex-col gap-2 p-2'>
      {/* 工具条 */}
      <div className='flex flex-wrap items-center gap-2'>
        <span className='text-sm font-bold'>Agent 配置 · 会话 #{sessionId}</span>
        <Badge variant='outline' className='rounded-sm'>伪 JSON · 契约驱动</Badge>
        {errors.length > 0 && (
          <Badge variant='outline' className='border-red-500/40 bg-red-500/10 text-red-600 dark:text-red-400'>
            {errors.length} 错误
          </Badge>
        )}
        {warnings.length > 0 && (
          <Badge variant='outline' className='border-amber-500/40 bg-amber-500/10 text-amber-600 dark:text-amber-400'>
            {warnings.length} 提示
          </Badge>
        )}
        <div className='ms-auto flex flex-wrap gap-2'>
          <Button size='sm' variant='outline' className='h-7 gap-1 text-xs' disabled={loading} onClick={refresh}>
            <RefreshCw className={cn('size-3.5', loading && 'animate-spin')} />
            刷新
          </Button>
          <Button size='sm' variant='outline' className='h-7 gap-1 text-xs' disabled={!loadedConfig} onClick={format}>
            <AlignLeft className='size-3.5' />
            格式化
          </Button>
          <Button size='sm' variant='outline' className='h-7 gap-1 text-xs' disabled={!loadedConfig} onClick={validate}>
            <CheckCircle2 className='size-3.5' />
            校验
          </Button>
          <Button
            size='sm'
            className='h-7 gap-1 text-xs'
            disabled={!loadedConfig || errors.length > 0 || !canWrite || submitting}
            title={!canWrite ? '当前角色无下发权限' : undefined}
            onClick={() => void submit()}
          >
            <Send className='size-3.5' />
            提交配置
          </Button>
        </div>
      </div>

      {/* 主区: 编辑器 + 右侧面板 */}
      <div className='flex min-h-0 flex-1 flex-col gap-2 lg:flex-row'>
        <div className='min-h-0 flex-1 overflow-hidden rounded-md border'>
          <PseudoJsonEditor value={text} onChange={setText} contract={contract} />
        </div>
        <aside className='flex min-h-[280px] w-full shrink-0 flex-col overflow-hidden rounded-md border lg:w-[430px]'>
          <Tabs
            value={sideTab}
            onValueChange={(v) => setSideTab(v as SideTab)}
            className='flex min-h-0 flex-1 flex-col gap-0'
          >
            <TabsList className='m-1.5 w-fit'>
              <TabsTrigger value='diff'>变更预览{diffEntries.length > 0 ? ` (${diffEntries.length})` : ''}</TabsTrigger>
              <TabsTrigger value='applied'>提交结果</TabsTrigger>
              <TabsTrigger value='startup'>只读信息</TabsTrigger>
            </TabsList>
            <div className='min-h-0 flex-1 overflow-y-auto'>
              {sideTab === 'diff' &&
                (parsed?.parseError ? (
                  <div className='m-2 flex gap-2 rounded-md border border-red-500/40 bg-red-500/5 p-2.5 text-xs leading-5'>
                    <TriangleAlert className='mt-0.5 size-3.5 shrink-0 text-red-500' />
                    <div>
                      <p className='font-medium text-red-600 dark:text-red-400'>存在语法错误，无法对比</p>
                      <p className='text-muted-foreground'>{parsed.parseError.message}</p>
                    </div>
                  </div>
                ) : (
                  <DiffPane entries={diffEntries} />
                ))}
              {sideTab === 'applied' && <AppliedPane applied={applied} submittedDomains={submittedDomains} />}
              {sideTab === 'startup' && <StartupPane startup={startup} />}
            </div>
          </Tabs>
        </aside>
      </div>

      {/* 刷新确认: 丢弃未提交修改 */}
      <ConfirmDialog
        open={refreshOpen}
        onOpenChange={setRefreshOpen}
        title='刷新将丢弃未提交的修改'
        desc={`当前编辑器与 Agent 回读配置有 ${diffEntries.length} 处差异，刷新会以 Agent 当前配置覆盖编辑内容。`}
        cancelBtnText='取消'
        confirmText='丢弃并刷新'
        destructive
        handleConfirm={() => {
          setRefreshOpen(false)
          void loadConfig()
        }}
      />

      {/* 校验问题弹窗(错误清单) */}
      <Dialog open={issuesOpen} onOpenChange={setIssuesOpen}>
        <DialogContent className='sm:max-w-2xl'>
          <DialogHeader>
            <DialogTitle>校验未通过（{errors.length} 项错误）</DialogTitle>
            <DialogDescription>以下字段发出了也不生效（类型/枚举/格式错误），请修复后再提交。</DialogDescription>
          </DialogHeader>
          <div className='max-h-80 space-y-2 overflow-y-auto'>
            {errors.map((e, i) => (
              <div key={i} className='rounded-md border border-red-500/30 bg-red-500/5 px-3 py-2 text-xs'>
                <code className='font-mono text-red-600 dark:text-red-400'>{e.path}</code>
                <p className='mt-0.5 text-muted-foreground'>{e.message}</p>
              </div>
            ))}
            {warnings.length > 0 && (
              <p className='pt-1 text-xs text-muted-foreground'>另有 {warnings.length} 项提示（钳位/未知键，不阻止提交）</p>
            )}
          </div>
          <DialogFooter>
            <Button variant='outline' onClick={() => setIssuesOpen(false)}>
              关闭
            </Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>
    </div>
  )
}
