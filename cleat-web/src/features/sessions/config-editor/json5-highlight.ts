/**
 * JSON5 语法的 CodeMirror StreamLanguage 高亮 + One Dark / One Light 双主题配色。
 * 不用 @codemirror/lang-json：其 Lezer 解析器不认注释/尾逗号/单引号，
 * 会在合法 JSON5 上标 parse error，污染编辑器（本编辑器全程 JSON5）。
 * StreamLanguage 无解析错误状态，纯着色 —— 注释/键/字符串/数字/字面量五类覆盖即可。
 *
 * 配色（peng ）：
 * - 暗色 = One Dark Pro：注释深绿斜体，键红、字符串绿、数字橙、字面量紫
 * - 亮色 = One Light 同构：注释深绿斜体，键红、字符串绿、数字橙褐、字面量青
 * - 主题跟随项目暗色模式（useNTTheme.isDark），切换实时生效
 */
import { StreamLanguage, HighlightStyle, syntaxHighlighting } from '@codemirror/language'
import { EditorView } from '@codemirror/view'
import { tags as t } from '@lezer/highlight'
import type { Extension } from '@codemirror/state'

interface Json5State {
  inBlock: boolean
}

const json5Stream = StreamLanguage.define<Json5State>({
  name: 'json5',
  startState(): Json5State {
    return { inBlock: false }
  },
  token(stream, state): string | null {
    // 块注释跨行续传
    if (state.inBlock) {
      if (stream.match(/^.*?\*\//)) state.inBlock = false
      else stream.skipToEnd()
      return 'comment'
    }
    if (stream.eatSpace()) return null

    const ch = stream.peek()
    if (ch === '/') {
      stream.next()
      const n = stream.peek()
      if (n === '/') { stream.skipToEnd(); return 'comment' }
      if (n === '*') { stream.next(); state.inBlock = true; return 'comment' }
      return null
    }
    // 单/双引号字符串（JSON5 允许单引号）；后面跟 `:` 的是键
    if (ch === '"' || ch === "'") {
      stream.next()
      while (!stream.eol()) {
        const c = stream.next()
        if (c === '\\') { stream.next(); continue }
        if (c === ch) break
      }
      return stream.match(/^\s*:/, false) ? 'property' : 'string'
    }
    // 数字：十进制 / 0x / .5 / 指数 / Infinity / NaN（JSON5 全集）
    if (stream.match(/^[+-]?(?:0[xX][0-9a-fA-F]+|\d+(?:\.\d*)?(?:[eE][+-]?\d+)?|\.\d+(?:[eE][+-]?\d+)?|Infinity|NaN)/)) {
      return 'number'
    }
    if (stream.match(/^(?:true|false|null)\b/)) return 'atom'
    if (stream.match(/^[{}[\],:]/)) return null
    stream.next()
    return null
  },
})

// ---- 语法配色：暗色 = One Dark Pro / 亮色 = One Light ----

const darkHighlight = HighlightStyle.define([
  { tag: t.comment, color: '#6a9955', fontStyle: 'normal' },    // 注释：深绿(正体, 不用斜体)
  { tag: t.propertyName, color: '#e06c75' },                    // 键：红
  { tag: t.string, color: '#98c379' },                          // 字符串：绿
  { tag: t.number, color: '#d19a66' },                          // 数字：橙
  { tag: t.atom, color: '#c678dd' },                            // true/false/null：紫
])

const lightHighlight = HighlightStyle.define([
  { tag: t.comment, color: '#2e7d32', fontStyle: 'normal' },   // 注释：深绿(正体, 不用斜体)
  { tag: t.propertyName, color: '#e45649' },                    // 键：红
  { tag: t.string, color: '#50a14f' },                          // 字符串：绿
  { tag: t.number, color: '#c18401' },                          // 数字：橙褐
  { tag: t.atom, color: '#0184bc' },                            // 字面量：青
])

// ---- 编辑器底色/边框/选区/行板：随主题切换（含 lint 面板暗色适配） ----
// 字体: 显式等宽栈 + 整数 13px/20px 行高 —— 之前的 12.5px 半像素字号在 Windows 下
// 走次像素渲染会"发虚", 且 CM 默认 generic monospace 可能落到 Courier New 细字形。

const darkTheme = EditorView.theme({
  '&': {
    color: '#abb2bf',
    backgroundColor: '#282c34',
    fontFamily: "'JetBrains Mono', 'Cascadia Code', Consolas, 'Courier New', monospace",
    fontSize: '13px',
  },
  '.cm-content': { caretColor: '#528bff', lineHeight: '20px', fontFamily: 'inherit' },
  // CM baseTheme 在 .cm-scroller 上硬编码 font-family: monospace(通用族→Windows 易落到细字形),
  // 必须在同一层覆盖, 只设 editor 根/内容层都压不住它。
  '.cm-scroller': { fontFamily: "'JetBrains Mono', 'Cascadia Code', Consolas, 'Courier New', monospace" },
  '&.cm-focused': { outline: 'none' },
  '&.cm-focused .cm-selectionBackground, .cm-selectionBackground, .cm-content ::selection': {
    backgroundColor: '#3e4451',
  },
  '.cm-gutters': { backgroundColor: '#282c34', color: '#495162', border: 'none' },
  '.cm-activeLine': { backgroundColor: '#2c313a' },
  '.cm-activeLineGutter': { backgroundColor: '#2c313a', color: '#abb2bf' },
  '.cm-foldGutter span': { color: '#73797f' },
  '.cm-panels': { backgroundColor: '#21252b', color: '#abb2bf' },
  '.cm-panels.cm-panels-bottom': { borderTop: '1px solid #181a1f' },
  '.cm-panel.cm-panel-lint ul li': { color: '#abb2bf' },
  '.cm-tooltip': { backgroundColor: '#21252b', border: '1px solid #181a1f', color: '#abb2bf' },
  '.cm-tooltip.cm-tooltip-autocomplete > ul': { backgroundColor: '#21252b', color: '#abb2bf' },
  '.cm-diagnostics': { fontSize: '12px' },
  // 滚动条：暗色（浏览器原生滚动条默认亮白，暗色下刺眼——peng ）
  '.cm-scroller::-webkit-scrollbar': { width: '10px', height: '10px' },
  '.cm-scroller::-webkit-scrollbar-track': { backgroundColor: '#21252b' },
  '.cm-scroller::-webkit-scrollbar-thumb': { backgroundColor: '#3e4451', borderRadius: '0' },
  '.cm-scroller::-webkit-scrollbar-thumb:hover': { backgroundColor: '#4d5566' },
  '.cm-scroller::-webkit-scrollbar-corner': { backgroundColor: '#21252b' },
}, { dark: true })

const lightTheme = EditorView.theme({
  '&': {
    color: '#383a42',
    backgroundColor: '#fafafa',
    fontFamily: "'JetBrains Mono', 'Cascadia Code', Consolas, 'Courier New', monospace",
    fontSize: '13px',
  },
  '.cm-content': { caretColor: '#526fff', lineHeight: '20px', fontFamily: 'inherit' },
  '.cm-scroller': { fontFamily: "'JetBrains Mono', 'Cascadia Code', Consolas, 'Courier New', monospace" },
  '&.cm-focused': { outline: 'none' },
  '&.cm-focused .cm-selectionBackground, .cm-selectionBackground, .cm-content ::selection': {
    backgroundColor: '#e5e5e6',
  },
  '.cm-gutters': { backgroundColor: '#fafafa', color: '#b0b1b7', border: 'none' },
  '.cm-activeLine': { backgroundColor: '#f0f0f1' },
  '.cm-activeLineGutter': { backgroundColor: '#f0f0f1', color: '#383a42' },
  '.cm-foldGutter span': { color: '#9da0a8' },
  '.cm-panels': { backgroundColor: '#f0f0f1', color: '#383a42' },
  '.cm-panels.cm-panels-bottom': { borderTop: '1px solid #e0e0e2' },
  '.cm-panel.cm-panel-lint ul li': { color: '#383a42' },
  '.cm-tooltip': { backgroundColor: '#ffffff', border: '1px solid #e0e0e2', color: '#383a42' },
  '.cm-diagnostics': { fontSize: '12px' },
  // 滚动条：亮色（直角，与工作区风格一致）
  '.cm-scroller::-webkit-scrollbar': { width: '10px', height: '10px' },
  '.cm-scroller::-webkit-scrollbar-track': { backgroundColor: '#f0f0f1' },
  '.cm-scroller::-webkit-scrollbar-thumb': { backgroundColor: '#d4d4d8', borderRadius: '0' },
  '.cm-scroller::-webkit-scrollbar-thumb:hover': { backgroundColor: '#bcbcc2' },
  '.cm-scroller::-webkit-scrollbar-corner': { backgroundColor: '#f0f0f1' },
}, { dark: false })

/** 编辑器扩展包：语法高亮 + 主题（随项目暗色模式切换） */
export function json5Theme(isDark: boolean): Extension[] {
  return [
    json5Stream,
    syntaxHighlighting(isDark ? darkHighlight : lightHighlight),
    isDark ? darkTheme : lightTheme,
  ]
}
