// 账户表单(真实对接 ): 显示当前操作员信息 + 自助修改密码。
// 后端语义(TeamServer operators.go): admin 可改任何人; operator/viewer 只能改自己 ——
// 本页只提供"改自己"(传 old_password 校验), 管理员改他人走「操作员」页。
import { useState } from 'react'
import { toast } from 'sonner'
import { changeOperatorPassword } from '@/lib/cleat-rest'
import { useAuthStore } from '@/stores/auth-store'
import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import { Input } from '@/components/ui/input'
import { Label } from '@/components/ui/label'

const ROLE_LABEL: Record<string, string> = {
  admin: '管理员 (admin)',
  operator: '操作员 (operator)',
  viewer: '观察者 (viewer)',
}

export function AccountForm() {
  const user = useAuthStore((s) => s.auth.user)
  const [oldPwd, setOldPwd] = useState('')
  const [newPwd, setNewPwd] = useState('')
  const [confirmPwd, setConfirmPwd] = useState('')
  const [busy, setBusy] = useState(false)

  const submit = async () => {
    if (!user) return
    if (!oldPwd || !newPwd) {
      toast.warning('请填写当前密码与新密码')
      return
    }
    if (newPwd !== confirmPwd) {
      toast.warning('两次输入的新密码不一致')
      return
    }
    setBusy(true)
    try {
      await changeOperatorPassword(user.username, oldPwd, newPwd)
      toast.success('密码已修改')
      setOldPwd('')
      setNewPwd('')
      setConfirmPwd('')
    } catch (e) {
      toast.error(`修改失败: ${(e as Error).message}`)
    } finally {
      setBusy(false)
    }
  }

  return (
    <div className='space-y-8'>
      {/* 当前操作员信息(只读) */}
      <div className='space-y-3'>
        <div className='space-y-1'>
          <Label>操作员</Label>
          <div className='flex items-center gap-2'>
            <span className='font-mono text-sm font-medium'>{user?.username || '—'}</span>
            <Badge variant='outline' className='rounded-sm text-[10px]'>
              {ROLE_LABEL[user?.role || ''] || user?.role || '—'}
            </Badge>
          </div>
          <p className='text-xs text-muted-foreground'>
            账户由管理员在「操作员」页创建与停用；此页可自助修改自己的登录密码。
          </p>
        </div>
      </div>

      {/* 修改密码 */}
      <div className='space-y-4'>
        <div className='space-y-1'>
          <Label htmlFor='old-password'>当前密码</Label>
          <Input
            id='old-password'
            type='password'
            autoComplete='current-password'
            value={oldPwd}
            onChange={(e) => setOldPwd(e.target.value)}
          />
        </div>
        <div className='space-y-1'>
          <Label htmlFor='new-password'>新密码</Label>
          <Input
            id='new-password'
            type='password'
            autoComplete='new-password'
            value={newPwd}
            onChange={(e) => setNewPwd(e.target.value)}
          />
        </div>
        <div className='space-y-1'>
          <Label htmlFor='confirm-password'>确认新密码</Label>
          <Input
            id='confirm-password'
            type='password'
            autoComplete='new-password'
            value={confirmPwd}
            onChange={(e) => setConfirmPwd(e.target.value)}
            onKeyDown={(e) => {
              if (e.key === 'Enter') void submit()
            }}
          />
        </div>
        <Button onClick={() => void submit()} disabled={busy}>
          {busy ? '提交中…' : '修改密码'}
        </Button>
      </div>
    </div>
  )
}
