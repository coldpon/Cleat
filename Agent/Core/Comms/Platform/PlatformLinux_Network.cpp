#include "Core/Comms/Platform/PlatformLinux_Network.hpp"

// 仅在实现文件中包含 POSIX 专用头文件
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <poll.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>   // TCP_NODELAY / TCP_KEEPIDLE / TCP_KEEPINTVL / TCP_KEEPCNT
#include <sys/socket.h>
#include <unistd.h>

namespace cleat {

    // ---------- Socket 生命周期 ----------
    INetworkOps::SocketHandle NetworkOpsLinux::CreateTcpSocket() {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            return -1;
        }
        // 选项与 Windows 实现对称(见 PlatformWin_Network.cpp::CreateTcpSocket), 均为 best-effort:
        // 失败不阻断建连, 仅退化为系统默认行为。
        // 禁用 Nagle: 小帧(心跳/命令)立即发出, 避免 ~40ms 延迟(交互类 C2 必需)
        // 注意 POSIX 默认 TCP_NODELAY=0(与 Go 的 net 包默认相反), 不设即为开 Nagle
        int nodelay = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        // 启用 TCP keepalive: 防空闲长连接被中间设备/系统按默认超时(通常 2h)回收
        int on = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
        // 短周期探测(30s 空闲探测 3 次): 无人值守长连接尽早探活, 与 Windows 侧参数对齐
        int idle = 30, intvl = 30, cnt = 3;
        ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
        ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
        ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
        return static_cast<SocketHandle>(fd);
    }

    void NetworkOpsLinux::CloseSocket(SocketHandle sock) {
        if (sock != -1) {
            ::close(static_cast<int>(sock));
        }
    }

    void NetworkOpsLinux::AbortSocket(SocketHandle sock) {
        // Linux: close 不唤醒阻塞 recv, 必须 shutdown(SHUT_RDWR) 让阻塞 recv 立即返回
        if (sock != -1) {
            ::shutdown(static_cast<int>(sock), SHUT_RDWR);
        }
    }

    // ---------- 连接 ----------
    bool NetworkOpsLinux::ConnectSocket(SocketHandle sock, const std::string& host, uint16_t port) {
        int s = static_cast<int>(sock);
        if (s < 0) return false;

        struct addrinfo hints {}, * result = nullptr;
        hints.ai_family = AF_INET; // 仅 IPv4, 与 Windows 实现保持一致
        hints.ai_socktype = SOCK_STREAM;

        std::string port_str = std::to_string(port);
        if (::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result) != 0) {
            return false;
        }

        // 非阻塞 connect + poll 超时: 阻塞 connect 在 SYN 重传下可挂 ~127s
        constexpr int kConnectTimeoutMs = 10000;
        bool connected = false;
        int orig_flags = ::fcntl(s, F_GETFL, 0);
        if (orig_flags >= 0)
            ::fcntl(s, F_SETFL, orig_flags | O_NONBLOCK);

        for (auto* addr = result; addr != nullptr; addr = addr->ai_next) {
            int rc = ::connect(s, addr->ai_addr, addr->ai_addrlen);
            if (rc == 0) {
                connected = true;
                break;
            }
            if (errno != EINPROGRESS)
                continue;
            struct pollfd pfd{};
            pfd.fd = s;
            pfd.events = POLLOUT;
            int pr = ::poll(&pfd, 1, kConnectTimeoutMs);
            if (pr > 0 && (pfd.revents & POLLOUT)) {
                int so_err = 0;
                socklen_t sl = sizeof(so_err);
                if (::getsockopt(s, SOL_SOCKET, SO_ERROR, &so_err, &sl) == 0 && so_err == 0) {
                    connected = true;
                    break;
                }
            }
        }
        ::freeaddrinfo(result);
        if (orig_flags >= 0)
            ::fcntl(s, F_SETFL, orig_flags);  // 恢复阻塞模式(上层依赖 SO_RCVTIMEO)
        return connected;
    }

    // ---------- 数据收发 ----------
    int NetworkOpsLinux::SendSocket(SocketHandle sock, const void* data, size_t len) {
        if (len == 0) return 0;
        if (data == nullptr) return -1;

        int s = static_cast<int>(sock);
        const char* buffer = static_cast<const char*>(data);
        size_t total_sent = 0;

        while (total_sent < len) {
            ssize_t sent = ::send(s, buffer + total_sent, len - total_sent, MSG_NOSIGNAL);
            if (sent < 0) {
                if (errno == EINTR)
                    continue;
                return (total_sent > 0) ? static_cast<int>(total_sent) : -1;
            }
            if (sent == 0) {
                return static_cast<int>(total_sent);
            }
            total_sent += static_cast<size_t>(sent);
        }
        return static_cast<int>(total_sent);
    }

    int NetworkOpsLinux::RecvSome(SocketHandle sock, void* buf, size_t len) {
        if (len == 0) return 0;
        if (buf == nullptr) return -1;
        int s = static_cast<int>(sock);
        for (;;) {
            const ssize_t received = ::recv(s, buf, len, 0);
            if (received < 0 && errno == EINTR) continue;
            return static_cast<int>(received);
        }
    }

    int NetworkOpsLinux::RecvSocket(SocketHandle sock, void* buf, size_t len) {
        if (len == 0) return 0;
        if (buf == nullptr) return -1;

        int s = static_cast<int>(sock);
        char* buffer = static_cast<char*>(buf);
        size_t total_received = 0;

        while (total_received < len) {
            ssize_t received = ::recv(s, buffer + total_received, len - total_received, 0);
            if (received < 0) {
                if (errno == EINTR)
                    continue;
                return (total_received > 0) ? static_cast<int>(total_received) : -1;
            }
            if (received == 0) {
                return static_cast<int>(total_received);
            }
            total_received += static_cast<size_t>(received);
        }
        return static_cast<int>(total_received);
    }

    // ---------- UDP 数据报(Phase 3: DNS/ICMP 通道; 本期 Linux 不构建, 代码保持正确) ----------
    INetworkOps::SocketHandle NetworkOpsLinux::CreateUdpSocket() {
        int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        return fd < 0 ? -1 : static_cast<SocketHandle>(fd);
    }

    int NetworkOpsLinux::SendTo(SocketHandle sock, const void* data, size_t len,
        const std::string& host, uint16_t port) {
        int s = static_cast<int>(sock);
        if (s < 0) return -1;

        struct addrinfo hints{}, * result = nullptr;
        hints.ai_family = AF_INET;          // IPv4 only for now
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;

        std::string port_str = std::to_string(port);
        if (::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result) != 0) {
            return -1;
        }
        // 仅取第一个解析地址; 多地址轮换留后续
        // getaddrinfo 的 ai_family 是建议非约束, 必须按 family 过滤再取
        struct sockaddr_in addr{};
        bool found = false;
        for (auto* it = result; it != nullptr; it = it->ai_next) {
            if (it->ai_family == AF_INET && it->ai_addr != nullptr
                && it->ai_addrlen >= static_cast<socklen_t>(sizeof(struct sockaddr_in))) {
                memcpy(&addr, it->ai_addr, sizeof(addr));
                found = true;
                break;
            }
        }
        ::freeaddrinfo(result);
        if (!found) return -1;

        for (;;) {
            const ssize_t sent = ::sendto(s, data, len, MSG_NOSIGNAL,
                reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr));
            if (sent < 0 && errno == EINTR)
                continue;
            return static_cast<int>(sent);
        }
    }

    int NetworkOpsLinux::RecvFrom(SocketHandle sock, void* buf, size_t len) {
        // 与 RecvSocket 语义对齐: len==0 视为空读成功返回 0(<=0 一律错误)
        if (buf == nullptr) return -1;
        if (len == 0) return 0;
        int s = static_cast<int>(sock);
        struct sockaddr_in from{};
        socklen_t from_len = sizeof(from);
        for (;;) {
            const ssize_t received = ::recvfrom(s, buf, len, 0,
                reinterpret_cast<struct sockaddr*>(&from), &from_len);
            if (received < 0 && errno == EINTR)
                continue;
            return static_cast<int>(received);
        }
    }

    // ---------- 监听(转发入口用) ----------
    bool NetworkOpsLinux::BindListen(SocketHandle sock, uint16_t port) {
        return BindListenOn(sock, port, "");
    }

    bool NetworkOpsLinux::BindListenOn(SocketHandle sock, uint16_t port,
        const std::string& bind_host) {
        int s = static_cast<int>(sock);
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
            if (::getaddrinfo(bind_host.c_str(), nullptr, &hints, &result) != 0
                || result == nullptr) {
                return false;
            }
            addr.sin_addr = reinterpret_cast<struct sockaddr_in*>(result->ai_addr)->sin_addr;
            ::freeaddrinfo(result);
        }
        if (::bind(s, reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr)) != 0) {
            return false;
        }
        return ::listen(s, SOMAXCONN) == 0;
    }

    INetworkOps::SocketHandle NetworkOpsLinux::AcceptSocket(SocketHandle listen_sock) {
        int c = ::accept(static_cast<int>(listen_sock), nullptr, nullptr);
        return c < 0 ? -1 : static_cast<SocketHandle>(c);
    }

    bool NetworkOpsLinux::ShutdownWrite(SocketHandle sock) {
        return ::shutdown(static_cast<int>(sock), SHUT_WR) == 0;
    }

    // ---------- 超时控制 ----------
    bool NetworkOpsLinux::SetSocketRecvTimeout(SocketHandle sock, int timeout_ms) {
        int s = static_cast<int>(sock);
        struct timeval tv {};
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        return ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
    }

    bool NetworkOpsLinux::SetSocketTimeout(SocketHandle sock, int timeout_ms) {
        int s = static_cast<int>(sock);
        struct timeval tv {};
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        bool ok = ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
        ok = ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0 && ok;
        return ok;
    }

    // ---------- 字节序转换 ----------
    uint32_t NetworkOpsLinux::Hton32(uint32_t host_value) {
        return htonl(host_value);
    }

    // ---------- 错误处理 ----------
    int NetworkOpsLinux::GetLastSocketError() {
        return errno;
    }

    int NetworkOpsLinux::GetSocketErrorTimeout() const {
        // recv/send 超时在 Linux 上表现为 EAGAIN (与 EWOULDBLOCK 同值)
        return EAGAIN;
    }

    bool NetworkOpsLinux::IsTransientRecvError(int err) const {
        // EWOULDBLOCK 与 EAGAIN 同值; EINTR(信号打断)属瞬态, 不应走"错误断线"
        return err == EWOULDBLOCK || err == EINTR;
    }

} // namespace cleat
