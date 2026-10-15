import { create } from 'zustand'
import { clearAuth, getUser, type CleatUser } from '@/lib/cleat-api'

// Cleat 版认证 store:
// 原 shadcn-admin 为 Clerk/模拟认证; 现改为 localStorage + Bearer(与 cleat-web 同机制),
// 保持 auth.reset() 调用点兼容(main.tsx / sign-out-dialog 无需改动)。

interface AuthState {
  auth: {
    user: CleatUser | null
    /** 清空登录态(token/user/role) */
    reset: () => void
  }
}

export const useAuthStore = create<AuthState>()((set) => ({
  auth: {
    user: getUser(),
    reset: () => {
      clearAuth()
      set((state) => ({ ...state, auth: { ...state.auth, user: null } }))
    },
  },
}))

/** 登录成功后刷新 store 中的用户信息(登录表单调用)。 */
export function refreshAuthUser() {
  useAuthStore.setState((state) => ({ ...state, auth: { ...state.auth, user: getUser() } }))
}
