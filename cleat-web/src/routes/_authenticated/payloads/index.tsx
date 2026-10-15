import { createFileRoute } from '@tanstack/react-router'
import { Payloads } from '@/features/payloads'

export const Route = createFileRoute('/_authenticated/payloads/')({
  component: Payloads,
})
