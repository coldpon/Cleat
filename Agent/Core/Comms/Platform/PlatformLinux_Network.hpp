#pragma once
#include "Core/Comms/Platform/INetworkOps.hpp"

namespace cleat {

    // Linux/POSIX network implementation.
    // Platform-specific APIs must only appear in this file and
    // PlatformLinux_Network.cpp. NOTE: no Linux build env available,
    // this implementation is not compile-verified on Linux yet.
    class NetworkOpsLinux : public INetworkOps {
    public:
        NetworkOpsLinux() = default;
        ~NetworkOpsLinux() override = default;

        // ---------- Socket lifecycle ----------
        SocketHandle CreateTcpSocket() override;
        void CloseSocket(SocketHandle sock) override;
        void AbortSocket(SocketHandle sock) override;

        // ---------- Connect ----------
        bool ConnectSocket(SocketHandle sock, const std::string& host, uint16_t port) override;

        // ---------- Listen (pivot entry) ----------
        bool BindListen(SocketHandle sock, uint16_t port) override;
        bool BindListenOn(SocketHandle sock, uint16_t port,
            const std::string& bind_host) override;
        SocketHandle AcceptSocket(SocketHandle listen_sock) override;

        // ---------- Data I/O ----------
        int SendSocket(SocketHandle sock, const void* data, size_t len) override;
        int RecvSocket(SocketHandle sock, void* buf, size_t len) override;
        int RecvSome(SocketHandle sock, void* buf, size_t len) override;
        bool ShutdownWrite(SocketHandle sock) override;

        // ---------- UDP 数据报(Phase 3: DNS/ICMP 通道; 本期 Linux 不构建, 声明保持正确) ----------
        SocketHandle CreateUdpSocket() override;
        int SendTo(SocketHandle sock, const void* data, size_t len,
            const std::string& host, uint16_t port) override;
        int RecvFrom(SocketHandle sock, void* buf, size_t len) override;

        // ---------- Timeout ----------
        bool SetSocketRecvTimeout(SocketHandle sock, int timeout_ms) override;
        bool SetSocketTimeout(SocketHandle sock, int timeout_ms) override;

        // ---------- Byte order ----------
        uint32_t Hton32(uint32_t host_value) override;

        // ---------- Errors ----------
        int GetLastSocketError() override;
        int GetSocketErrorTimeout() const override;
        bool IsTransientRecvError(int err) const override;
    };

} // namespace cleat
