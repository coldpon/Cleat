import { createFileRoute } from '@tanstack/react-router'
import { Modules } from '@/features/modules'

// 模块仓库(真实对接): /api/modules 列表/上传/删除
export const Route = createFileRoute('/_authenticated/modules/')({
  component: Modules,
})
