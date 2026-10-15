// auth-store 测试(Cleat 版,  重写): 原 shadcn-admin 模板测试针对的是
// Clerk 版 store(accessToken/setUser 等), 与 Cleat 改造后的 localStorage + Bearer
// 模型不符, 已全部失效。此处按现有 API(user / reset)重写, 并覆盖持久化读回语义。
import { getToken, getUser, saveAuth } from '@/lib/cleat-api'
import { beforeEach, describe, expect, it, vi } from 'vitest'

async function importAuthStore() {
  const { useAuthStore } = await import('./auth-store')
  return useAuthStore
}

describe('useAuthStore (Cleat 版: localStorage + Bearer)', () => {
  beforeEach(() => {
    localStorage.clear()
    vi.resetModules()
  })

  it('未持久化任何凭据时以未登录态启动', async () => {
    const useAuthStore = await importAuthStore()
    expect(useAuthStore.getState().auth.user).toBeNull()
  })

  it('持久化的 token/user/role 在新 store 实例上会被读回', async () => {
    saveAuth('session-token', 'admin', 'admin')
    vi.resetModules()
    const useAuthStore = await importAuthStore()

    expect(getToken()).toBe('session-token')
    expect(useAuthStore.getState().auth.user).toEqual({ username: 'admin', role: 'admin' })
  })

  it('reset 清空内存态与持久化(localStorage)', async () => {
    saveAuth('will-be-cleared', 'op', 'operator')
    vi.resetModules()
    const useAuthStore = await importAuthStore()
    expect(useAuthStore.getState().auth.user?.username).toBe('op')

    useAuthStore.getState().auth.reset()

    expect(useAuthStore.getState().auth.user).toBeNull()
    expect(getToken()).toBeNull()
    expect(getUser()).toBeNull()

    vi.resetModules()
    const afterReload = await importAuthStore()
    expect(afterReload.getState().auth.user).toBeNull()
  })
})
