import { createFileRoute } from '@tanstack/react-router'
import { Listeners } from '@/features/listeners'

export const Route = createFileRoute('/_authenticated/listeners/')({
  component: Listeners,
})
