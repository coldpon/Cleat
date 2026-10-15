import { createFileRoute, redirect } from '@tanstack/react-router'
import { getToken } from '@/lib/cleat-api'
import { AuthenticatedLayout } from '@/components/layout/authenticated-layout'

export const Route = createFileRoute('/_authenticated')({
  // 认证守卫: 无 token 直接跳登录页, 并记录回跳地址。
  beforeLoad: ({ location }) => {
    if (!getToken()) {
      throw redirect({
        to: '/sign-in',
        search: { redirect: location.href },
      })
    }
  },
  component: AuthenticatedLayout,
})
