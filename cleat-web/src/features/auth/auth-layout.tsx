// 认证页布局(登录): 全屏背景图(自有素材) + 渐晕保证前景可读。
import loginBg from '@/assets/login-bg.png'
import { CleatLogo } from '@/assets/cleat-logo'

type AuthLayoutProps = {
  children: React.ReactNode
}

export function AuthLayout({ children }: AuthLayoutProps) {
  return (
    <div className='relative grid h-svh items-center justify-center overflow-hidden'>
      {/* 背景图 + 渐晕(前景白字/卡片在任何区域都可读) */}
      <img src={loginBg} alt='' aria-hidden className='absolute inset-0 h-full w-full object-cover' />
      <div className='absolute inset-0 bg-gradient-to-b from-black/25 via-black/10 to-black/45' />

      <div className='relative mx-auto flex w-full flex-col justify-center space-y-2 py-8 sm:p-8'>
        <div className='mb-4 flex items-center justify-center gap-2'>
          <CleatLogo className='size-9 rounded-md' />
          <h1 className='text-xl font-medium tracking-tight text-white drop-shadow-[0_1px_4px_rgba(0,0,0,0.85)]'>
            CLEAT · C2 Console
          </h1>
        </div>
        {children}
      </div>
    </div>
  )
}
