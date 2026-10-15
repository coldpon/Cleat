#pragma once
// PlatformWin_Wss: WinHTTP 原生 WebSocket 实现(Windows 8.1+)。
// - 系统库零依赖(WinHTTP.dll), Schannel 自动 TLS, 系统证书存储校验
// - WS API 要求是**异步会话**(WINHTTP_FLAG_ASYNC): 升级握手与收发完成均经
//   状态回调投递, 本文件用"每方向一个自动复位事件 + 单在飞操作"模型收口
// - UA/代理/证书口径与 PlatformWin_Http 同源(AgentConfig http 域)
#pragma once

#include "Core/Comms/Transports/WebSocket/IWssOps.hpp"

namespace cleat {

    // 工厂实现位于 PlatformWin_Wss.cpp(CMake 按平台过滤)
    std::unique_ptr<IWssOps> CreateWssOps(INetworkOps& net_ops);

} // namespace cleat
