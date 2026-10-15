// Cleat 品牌徽记(图源: 老前端 public/pwa-192x192.png, 已拷入本仓 public/images/cleat-logo.png)。
// 近黑底白色蟠纹圆徽 —— 深/浅色背景上均可直接使用。
import { cn } from '@/lib/utils'

export function CleatLogo({ className }: { className?: string }) {
  return (
    <img
      src='/images/cleat-logo.png'
      alt='Cleat'
      draggable={false}
      className={cn('size-5 shrink-0 rounded-sm object-cover', className)}
    />
  )
}
