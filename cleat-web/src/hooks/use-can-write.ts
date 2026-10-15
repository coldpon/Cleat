// 写操作门控: admin / operator 才具备命令下发能力(与老 cleat-web useCanWrite 对齐)。
// 前端隐藏/禁用只是体验 —— 真正的边界在服务端(TeamServer 对写接口有独立校验)。
import { useAuthStore } from '@/stores/auth-store'

export function useCanWrite(): boolean {
  const role = useAuthStore((s) => s.auth.user?.role)
  return role === 'admin' || role === 'operator'
}
