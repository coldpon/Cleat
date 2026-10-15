#include "CommandDispatcher.hpp"
#include "Common/Protocol/Packet.hpp"
#include <algorithm>
#include <exception>
#include <string>

namespace cleat {

	void CommandDispatcher::RegisterHandler(const std::string& cmd, HandlerFactory factory,
		uint8_t priority) {
		std::lock_guard<std::mutex> lock(mutex_);
		handlers_[cmd] = std::move(factory);
		if (priority == kPriorityUnset) {
			priorities_.erase(cmd);
		}
		else {
			priorities_[cmd] = priority;
		}
	}

	void CommandDispatcher::UnregisterHandler(const std::string& cmd) {
		std::lock_guard<std::mutex> lock(mutex_);
		handlers_.erase(cmd);
		priorities_.erase(cmd);
	}

	uint8_t CommandDispatcher::PriorityOf(const std::string& cmd) const {
		std::lock_guard<std::mutex> lock(mutex_);
		const auto it = priorities_.find(cmd);
		return (it == priorities_.end()) ? kPriorityUnset : it->second;
	}

	std::vector<CommandDispatcher::CommandInfo> CommandDispatcher::ListHandlers() const {
		std::lock_guard<std::mutex> lock(mutex_);
		std::vector<CommandInfo> out;
		out.reserve(handlers_.size());
		for (const auto& entry : handlers_) {
			CommandInfo info;
			info.cmd = entry.first;
			const auto it = priorities_.find(entry.first);
			if (it != priorities_.end()) info.priority = it->second;
			out.push_back(std::move(info));
		}
		// 字典序稳定输出: 自述响应可 diff, 不随 unordered_map 桶序抖动
		std::sort(out.begin(), out.end(),
			[](const CommandInfo& a, const CommandInfo& b) { return a.cmd < b.cmd; });
		return out;
	}

	ResourceKey CommandDispatcher::ResourceOf(const std::string& cmd, const CommandRequest& req) const {
		HandlerFactory factory;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			const auto it = handlers_.find(cmd);
			if (it == handlers_.end()) return ResourceKey{};
			factory = it->second;
		}
		try {
			const auto handler = factory();
			return handler ? handler->ResourceOf(req) : ResourceKey{};
		}
		catch (...) {
			return ResourceKey{};
		}
	}

	nlohmann::json CommandDispatcher::Dispatch(const std::string& cmd, const CommandRequest& req) {
		try {
			// 查找处理器工厂
			HandlerFactory factory;
			{
				std::lock_guard<std::mutex> lock(mutex_);
				auto it = handlers_.find(cmd);
				if (it == handlers_.end()) {
					return Packet::Error("Unknown command: " + cmd);
				}
				factory = it->second;
			}

			// 创建处理器实例
			auto handler = factory();
			// (第十七轮): 工厂产品可能为空 —— 空指针解引用**不是异常**, 下面的
			// catch 抓不到, 会变成访问违例并被 RunWorkerGuarded 转成"崩溃 + 自重启"
			//(表象是链路随机抖动, 极难定位)。与同文件 ResourceOf(:42) 的判空口径对齐;
			// 当前注册点虽无返回 nullptr 的工厂, 但契约不落在类型里就会被未来打破。
			if (!handler) {
				return Packet::Error("Handler factory returned null for: " + cmd);
			}
			// 执行
			return handler->Execute(req);
		}
		catch (const std::exception& e) {
			// 捕获所有标准异常
			return Packet::Error(std::string("Handler exception: ") + e.what());
		}
		catch (...) {
			// 捕获非标准异常（如未知异常）
			return Packet::Error("Unknown handler exception");
		}
	}

} // namespace cleat