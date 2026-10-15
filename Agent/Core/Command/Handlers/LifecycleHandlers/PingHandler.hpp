#pragma once

#include "Core/Command/Handlers/IHandler.hpp"
#include "Common/ThirdParty/json.hpp"
#include <vector>

namespace cleat {

	class PingHandler : public IHandler {
	public:
		nlohmann::json Execute(const CommandRequest& req) override;
	};

} // namespace cleat