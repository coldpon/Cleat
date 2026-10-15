import { createFileRoute } from '@tanstack/react-router'
import { Topology } from '@/features/topology'

export const Route = createFileRoute('/_authenticated/topology/')({
  component: Topology,
})
