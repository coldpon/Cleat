#pragma once
#include "Core/Command/Handlers/IHandler.hpp"
#include"Components/Commands/SystemInfo/Provider/SystemInfoProvider.hpp"
#include "Common/ThirdParty/json.hpp"

namespace cleat {

	class SystemInfoHandler : public IHandler {
	private:
		SystemInfoProvider& provider_;
	public:
		explicit SystemInfoHandler(SystemInfoProvider& provider);
		nlohmann::json Execute(const CommandRequest& req) override;

	
	};

} // namespace cleat