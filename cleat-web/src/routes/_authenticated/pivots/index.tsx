import { createFileRoute } from '@tanstack/react-router'
import { Pivots } from '@/features/pivots'

export const Route = createFileRoute('/_authenticated/pivots/')({
  component: Pivots,
})
