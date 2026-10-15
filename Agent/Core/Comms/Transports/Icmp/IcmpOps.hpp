// Core/Comms/Transports/Icmp/IcmpOps.hpp
// ICMP 回声窄接口(Phase 3):
// 平台细节在 PlatformWin_Icmp(IcmpSendEcho, 免管理员)实现; Linux ping socket 延后。
// Agent 无需 raw socket: 出站=echo 请求 payload, 下行=echo 应答 payload。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cleat {

	class IIcmpOps {
	public:
		virtual ~IIcmpOps() = default;

		// 发送 echo 请求(target 为 IP; payload = 上行分片),
		// 等待应答并把应答 payload(= 下行分片)填入 reply_payload。
		// 超时(timeout_ms)/失败返回 false。
		virtual bool Echo(const std::string& target,
			const std::vector<uint8_t>& payload,
			std::vector<uint8_t>& reply_payload, int timeout_ms) = 0;

		virtual void Reset() = 0;
	};

	// 工厂(平台实现文件定义; Linux 延后)
	std::unique_ptr<IIcmpOps> CreateIcmpOps();

} // namespace cleat
