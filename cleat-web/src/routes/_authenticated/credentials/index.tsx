import { createFileRoute } from '@tanstack/react-router'
import { Credentials } from '@/features/credentials'

export const Route = createFileRoute('/_authenticated/credentials/')({
  component: Credentials,
})
