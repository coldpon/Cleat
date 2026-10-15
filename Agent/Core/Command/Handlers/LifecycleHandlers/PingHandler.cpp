#include "PingHandler.hpp"
#include "Common/Protocol/Packet.hpp"
#include <chrono>
namespace cleat {

	nlohmann::json PingHandler::Execute(const CommandRequest&) {
		// 简单返回 Pong，可携带时间戳或客户端信息
		nlohmann::json data;
		data["response"] = "Pong";
		data["timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::system_clock::now().time_since_epoch()
		).count();
		return Packet::Ok(std::move(data));
	}

} // namespace cleat