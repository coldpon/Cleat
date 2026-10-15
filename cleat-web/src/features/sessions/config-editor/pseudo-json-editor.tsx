// 伪 JSON 编辑器(CodeMirror 封装, 移植自 cleat-web configEditor/PseudoJsonEditor):
// - JSON5 StreamLanguage 高亮(json5-highlight.ts, 不用 lang-json 的原因见其头注释)
// - linter 实时诊断: JSON5 解析错误(精确行列) + 契约/域内校验错误(best-effort 行定位)
//   诊断仅在编辑器内联展示, 不向父组件上报 —— 提交/校验按钮由父组件自行跑 parsePseudoJson。
import { useMemo } from 'react'
import CodeMirror from '@uiw/react-codemirror'
import { linter, type Diagnostic } from '@codemirror/lint'
import { useTheme } from '@/context/theme-provider'
import type { ContractKeySpec } from '@/lib/agent-config-schema'
import { json5Theme } from './json5-highlight'
import { parsePseudoJson, findFieldLine } from './pseudo-json'

interface Props {
  value: string
  onChange: (v: string) => void
  contract?: ContractKeySpec[]
}

export function PseudoJsonEditor({ value, onChange, contract }: Props) {
  // 主题跟随项目亮/暗模式(与全局 ThemeProvider 同源, 切换实时生效)
  const { resolvedTheme } = useTheme()
  const isDark = resolvedTheme === 'dark'

  const extensions = useMemo(() => {
    const lintExt = linter(
      (view) => {
        const text = view.state.doc.toString()
        const doc = parsePseudoJson(text, contract)
        const diags: Diagnostic[] = []
        const total = text.length
        if (doc.parseError) {
          let pos = 0
          if (doc.parseError.line > 1 || doc.parseError.column > 1) {
            const lineNo = Math.min(Math.max(1, doc.parseError.line), view.state.doc.lines)
            pos = view.state.doc.line(lineNo).from + Math.max(0, doc.parseError.column - 1)
          }
          pos = Math.min(Math.max(0, pos), total)
          diags.push({
            from: pos,
            to: Math.min(pos + 1, total),
            severity: 'error',
            message: `语法错误：${doc.parseError.message}`,
          })
        }
        for (const issue of doc.issues) {
          const ln = issue.line ?? findFieldLine(text, issue.path) ?? 1
          const lineInfo = view.state.doc.line(Math.min(Math.max(1, ln), view.state.doc.lines))
          diags.push({
            from: lineInfo.from,
            to: lineInfo.to,
            severity: issue.severity,
            message: `${issue.path}：${issue.message}`,
          })
        }
        return diags
      },
      { delay: 400 }
    )
    return [...json5Theme(isDark), lintExt]
  }, [isDark, contract])

  return (
    <CodeMirror
      value={value}
      height='100%'
      style={{ height: '100%', fontSize: 13 }}
      // theme="none": 禁用 @uiw 默认 light 主题——它会把我们的暗色背景盖成白底
      // (EditorView.theme 背景战争, 实测复现); 亮/暗完全由 json5Theme(isDark) 决定
      theme='none'
      extensions={extensions}
      onChange={onChange}
      basicSetup={{
        foldGutter: true,
        autocompletion: false, // 不做语法自动补全
        highlightActiveLine: true,
        bracketMatching: true,
        closeBrackets: true,
      }}
    />
  )
}
