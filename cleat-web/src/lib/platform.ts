// 会话平台粗判(公共): 多处复用 —— 摄像头/键盘/录音/服务/驱动/软件/窗口等能力的裁剪依据。
// 与老版 SessionDetailPage 的 isLinux 判据一致; Windows/macOS 均按"非 Linux"处理(能力由 Agent 侧再判)。
export function isLinuxOs(os: string): boolean {
  const o = (os || '').toLowerCase()
  return ['linux', 'ubuntu', 'debian', 'kali', 'centos', 'arch', 'alpine'].some((k) => o.includes(k))
}
