#include "SystemInfoHandler.hpp"
#include "Common/Protocol/Packet.hpp"

namespace cleat {

	SystemInfoHandler::SystemInfoHandler(SystemInfoProvider& provider)
		: provider_(provider) {
	}

	nlohmann::json SystemInfoHandler::Execute(const CommandRequest&) {
		auto info = provider_.GetSystemInfo();

		nlohmann::json data;
		data["hostname"] = info.hostname;
		data["os_name"] = info.os_name;
		data["os_version"] = info.os_version;
		data["architecture"] = info.architecture;
		data["total_ram_bytes"] = info.total_ram_bytes;
		data["cpu_cores"] = info.cpu_cores;
		data["cpu_brand"] = info.cpu_brand;
		data["local_ip"] = info.local_ip;
		data["pid"] = info.pid;
		data["process_name"] = info.process_name;
		data["privilege"] = info.privilege;
		return Packet::Ok(std::move(data));
	}

} // namespace cleat