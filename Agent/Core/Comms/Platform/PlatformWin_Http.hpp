// Core/Comms/Platform/PlatformWin_Http.hpp
// WinHTTP 实现(Phase 1, Windows;  随 IHttpOps 归位 Platform 层):
// - 系统库零依赖(WinHTTP.dll), Schannel 自动 TLS, 系统证书存储校验
// - 显式 NO_PROXY(防企业 netsh winhttp 劫持), 禁重定向(30x 劫持防护)
// - 超时全部显式设置
// - 同步模式 + 显式超时, 满足上层单线程 poll
#pragma once

#include "Core/Comms/Platform/IHttpOps.hpp"

namespace cleat {

	// 工厂实现位于 PlatformWin_Http.cpp(CMake 按平台过滤)
	std::unique_ptr<IHttpOps> CreateHttpOps();

} // namespace cleat
