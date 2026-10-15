import { createFileRoute } from '@tanstack/react-router'
import { Operators } from '@/features/operators'

export const Route = createFileRoute('/_authenticated/operators/')({
  component: Operators,
})
