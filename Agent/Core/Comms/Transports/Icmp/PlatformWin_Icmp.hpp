// Core/Comms/Transports/Icmp/PlatformWin_Icmp.hpp
// ICMP 回声实现(Windows): IcmpSendEcho(iphlpapi, 免管理员, 无需 raw socket)。
#pragma once

#include "Core/Comms/Transports/Icmp/IcmpOps.hpp"

#include <windows.h>
#include <iphlpapi.h>   // 先引入(定义 IPAddr 等基础类型)
#include <icmpapi.h>    // IcmpCreateFile/IcmpSendEcho/IcmpCloseHandle

#include <memory>
#include <string>
#include <vector>

namespace cleat {

	class PlatformWin_Icmp : public IIcmpOps {
	public:
		PlatformWin_Icmp();
		~PlatformWin_Icmp() override;

		// 发送 echo 请求(target 为 IPv4; payload = 上行分片), 等待应答并
		// 把应答 payload(= 下行分片)填入 reply_payload; 超时/失败返回 false。
		bool Echo(const std::string& target, const std::vector<uint8_t>& payload,
			std::vector<uint8_t>& reply_payload, int timeout_ms) override;

		void Reset() override;

	private:
		HANDLE handle_ = INVALID_HANDLE_VALUE;
	};

	// 工厂(平台文件提供同名函数, CMake 按平台过滤选择实现)
	std::unique_ptr<IIcmpOps> CreateIcmpOps();

} // namespace cleat
