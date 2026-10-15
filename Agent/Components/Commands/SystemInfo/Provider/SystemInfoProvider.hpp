#pragma once
#include "Components/Commands/SystemInfo/Platform/ISystemInfo.hpp"
#include"Common/ThirdParty/json.hpp"
namespace cleat {

	class SystemInfoProvider {
	public:
		explicit SystemInfoProvider(ISystemInfo& system_info);
		SystemInfo GetSystemInfo() const;
		nlohmann::json ToJson() const;
	private:
		
		ISystemInfo& system_info_;
	};

} // namespace cleat