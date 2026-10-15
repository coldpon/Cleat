import { createFileRoute, redirect } from '@tanstack/react-router'

// 设置根路径 → 默认进「账户」子页(原模板的 Profile 页已删除)
export const Route = createFileRoute('/_authenticated/settings/')({
  beforeLoad: () => {
    throw redirect({ to: '/settings/account' })
  },
})
