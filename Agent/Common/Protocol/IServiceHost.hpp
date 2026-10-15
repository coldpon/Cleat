#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace cleat {

	class IServiceHost {
	public:
		virtual ~IServiceHost() = default;

		virtual bool IsNetworkConnected() const = 0;
		virtual void SendData(const std::vector<uint8_t>& data) = 0;
		// 日志统一走 Common/Logger(LOG_* 宏), 不再经 IServiceHost 转发
	};

} // namespace cleat