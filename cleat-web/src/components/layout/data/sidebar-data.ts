// Cleat 侧栏导航(对齐老版 12 模块: 作战/武器/系统 三组)。
import {
  Blocks,
  FileText,
  KeyRound,
  LayoutDashboard,
  Monitor,
  Network,
  Package,
  Radio,
  ScrollText,
  Target,
  Users,
  Video,
  Waypoints,
} from 'lucide-react'
import { type SidebarData } from '../types'
import { CleatLogo } from '@/assets/cleat-logo'

export const sidebarData: SidebarData = {
  user: {
    name: 'admin',
    email: '',
    avatar: '',
  },
  teams: [
    {
      name: 'CLEAT',
      logo: CleatLogo,
      plan: 'C2 Console',
    },
  ],
  navGroups: [
    {
      title: '作战',
      items: [
        { title: '总览', url: '/', icon: LayoutDashboard },
        { title: '会话', url: '/sessions', icon: Monitor },
        { title: '目标', url: '/targets', icon: Target },
        { title: '凭据', url: '/credentials', icon: KeyRound },
        { title: '监听器', url: '/listeners', icon: Radio },
        { title: '隧道', url: '/pivots', icon: Waypoints },
        { title: '拓扑图', url: '/topology', icon: Network },
        { title: '音视频监控', url: '/av-monitor', icon: Video },
      ],
    },
    {
      title: '武器',
      items: [
        { title: '载荷生成', url: '/payloads', icon: Package },
        { title: '模块', url: '/modules', icon: Blocks },
      ],
    },
    {
      title: '系统',
      items: [
        { title: '事件日志', url: '/events', icon: FileText },
        { title: '操作员', url: '/operators', icon: Users },
        { title: '审计', url: '/audit', icon: ScrollText },
      ],
    },
  ],
}
