#include "TacticManager.hpp"
#include "Common/Logger/ErrLog.hpp"
#include "Common/Logger/Logger.hpp"

namespace cleat {

	TacticManager& TacticManager::Instance() {
		static TacticManager instance;
		return instance;
	}

	void TacticManager::RegisterModule(std::unique_ptr<ITacticModule> module) {
		std::string name = module->GetName();
		if (modules_.find(name) != modules_.end()) {
			LOG_WARN("TacticManager: 模块 " << name << " 已注册，将被覆盖");
		}
		modules_[name] = std::move(module);
		LOG_DEBUG("TacticManager: 注册模块 " << name);
	}

	std::vector<std::string> TacticManager::ApplyConfig(const nlohmann::json& config) {
		std::vector<std::string> failed;
		if (!config.is_object()) {
			CLEAT_LOGE(ErrDom::Tac, ErrCode::InvalidParam, "TacticManager: 配置不是 JSON 对象");
			return { "config" };
		}

		for (auto& pair : modules_) {
			const std::string& name = pair.first;
			if (config.contains(name) && config[name].is_object()) {
				if (!pair.second->Apply(config[name])) {
					CLEAT_LOGE(ErrDom::Tac, ErrCode::InternalFailed, "TacticManager: 模块 " << name << " 应用配置失败");
					failed.push_back(name);
				}
				else {
					LOG_DEBUG("TacticManager: 应用模块 " << name << " 配置");
				}
			}
			else {
				LOG_DEBUG("TacticManager: 模块 " << name << " 无配置或配置非对象，跳过");
			}
		}
		return failed;
	}

	void TacticManager::SetDataSender(std::function<void(const std::vector<uint8_t>&)> sender) {
		dataSender_ = std::move(sender);
	}

	void TacticManager::SendData(const nlohmann::json& payload) const {
		if (!dataSender_) {
			CLEAT_LOGE(ErrDom::Tac, ErrCode::InternalFailed, "TacticManager: 数据发送回调未注入，丢弃推送");
			return;
		}
		std::string jsonStr = payload.dump();
		std::vector<uint8_t> bytes(jsonStr.begin(), jsonStr.end());
		dataSender_(bytes);
	}

	std::vector<std::string> TacticManager::GetModuleNames() const {
		std::vector<std::string> names;
		for (const auto& pair : modules_) {
			names.push_back(pair.first);
		}
		return names;
	}

} // namespace cleat