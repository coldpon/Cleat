// 固定行高列表的轻量虚拟滚动(零依赖) —— 大列表(300+)性能问题的标准解法:
// 滚动容器内只渲染"可见行 ± overscan", 上下用等高占位行撑住滚动条。
// 用法(表格): 调用方自持滚动容器 ref(传给本钩子), tbody 首尾插 <TableRow style={{height: pad}}> 占位,
// 中间渲染 slice(start, end); 配合 <Table className='table-fixed'> 保证列宽稳定不随滚动抖动。
// 约定: 行高必须恒定(内容单行 truncate, 不换行)。
// 注: ref 由调用方持有并从形参传入 —— 若由钩子返回, react-hooks/refs 会把返回值整体
//     视为"含 ref 值", 渲染期读取 start/padTop 等派生值会被误判为渲染期摸 ref。
import { useCallback, useEffect, useState } from 'react'

export function useVirtualRows(
  count: number,
  rowHeight: number,
  scrollRef: React.RefObject<HTMLDivElement | null>,
  overscan = 10
) {
  const [range, setRange] = useState({ start: 0, end: Math.min(count, overscan * 2 + 10) })

  const recompute = useCallback(() => {
    const el = scrollRef.current
    if (!el) return
    const start = Math.max(0, Math.floor(el.scrollTop / rowHeight) - overscan)
    const end = Math.min(count, Math.ceil(el.clientHeight / rowHeight) + start + overscan * 2)
    setRange((prev) => (prev.start === start && prev.end === end ? prev : { start, end }))
  }, [count, rowHeight, overscan, scrollRef])

  useEffect(() => {
    recompute()
  }, [recompute])

  useEffect(() => {
    const el = scrollRef.current
    if (!el) return
    const onScroll = () => recompute()
    el.addEventListener('scroll', onScroll, { passive: true })
    const ro = new ResizeObserver(() => recompute())
    ro.observe(el)
    return () => {
      el.removeEventListener('scroll', onScroll)
      ro.disconnect()
    }
  }, [recompute, scrollRef])

  return {
    start: range.start,
    end: range.end,
    /** 列表顶部占位高度(px) */
    padTop: range.start * rowHeight,
    /** 列表底部占位高度(px) */
    padBottom: Math.max(0, (count - range.end) * rowHeight),
  }
}
