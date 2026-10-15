#include "SystemInfoProvider.hpp"

namespace cleat {

	SystemInfoProvider::SystemInfoProvider(ISystemInfo& system_info)
		: system_info_(system_info) {
	}

	SystemInfo SystemInfoProvider::GetSystemInfo() const {
		return system_info_.GetSystemInfo();
	}
    nlohmann::json SystemInfoProvider::ToJson() const {
        auto info = GetSystemInfo();
        return {
            {"hostname", info.hostname},
            {"os_name", info.os_name},
            {"os_version", info.os_version},
            {"architecture", info.architecture},
            {"total_ram_bytes", info.total_ram_bytes},
            {"cpu_cores", info.cpu_cores},
            {"cpu_brand", info.cpu_brand},
            {"local_ip", info.local_ip},
            {"pid", info.pid},
            {"process_name", info.process_name},
            {"privilege", info.privilege}
        };
    }

} // namespace cleat