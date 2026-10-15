#pragma once

#include <cstdint>
#include <string>
#include<memory>
namespace cleat {

    /**
     * @brief 网络操作接口（纯虚）
     *
     * 职责：封装所有平台相关的网络 API（socket、connect、send、recv 等）。
     * 上层模块（如 CommsManager）只依赖此接口，不直接调用 Winsock/POSIX。
     */
    class INetworkOps {
    public:
        virtual ~INetworkOps() = default;

        // Socket 句柄类型（确保 32/64 位兼容）
        using SocketHandle = intptr_t;

        // ---------- Socket 生命周期 ----------
        virtual SocketHandle CreateTcpSocket() = 0;
        virtual void CloseSocket(SocketHandle sock) = 0;
        // 断连前中止阻塞 IO: Linux 上 close 不唤醒阻塞 recv(会卡满 SO_RCVTIMEO),
        // 需先 shutdown(SHUT_RDWR); Windows 上 closesocket 自带唤醒, 实现可为空操作。
        // 语义: 调用后该 fd 不再接收/发送数据, 随后的 CloseSocket 应立即返回。
        virtual void AbortSocket(SocketHandle sock) = 0;

        // ---------- 连接 ----------
        virtual bool ConnectSocket(SocketHandle sock, const std::string& host, uint16_t port) = 0;

        // ---------- 监听(转发入口用) ----------
        // 绑定并监听 0.0.0.0:port; 返回 true 表示就绪
        virtual bool BindListen(SocketHandle sock, uint16_t port) = 0;
        // 绑定到指定 IPv4 地址并监听(bind_host 为空或 "0.0.0.0" 时绑所有网卡);
        // 返回 true 表示就绪
        virtual bool BindListenOn(SocketHandle sock, uint16_t port,
            const std::string& bind_host) = 0;
        // 接受一个入站连接; 返回新 socket(失败返回 -1)
        virtual SocketHandle AcceptSocket(SocketHandle listen_sock) = 0;

        // ---------- 数据收发 ----------
        // 半关闭写方向: 通知对端数据已发完(优雅关闭, 避免 RST 丢数据)
        virtual bool ShutdownWrite(SocketHandle sock) = 0;
        // 返回实际收发字节数；<=0 表示错误
        virtual int SendSocket(SocketHandle sock, const void* data, size_t len) = 0;
        virtual int RecvSocket(SocketHandle sock, void* buf, size_t len) = 0;
        // 流式读: 至多一次 recv, 有数据即回(不等待凑满 len)。交互式短帧/中继/HTTP 读循环
        // 必须用它 —— RecvSocket 收满 len 才回, 会让小帧等到 SO_RCVTIMEO 到期才被交付。
        // 语义: >0 实际字节; 0 对端 FIN; <0 错误(超时/瞬态看 GetLastSocketError)。
        virtual int RecvSome(SocketHandle sock, void* buf, size_t len) = 0;

        // ---------- UDP 数据报(Phase 3: DNS/ICMP 通道) ----------
        virtual SocketHandle CreateUdpSocket() = 0;
        // 发送数据报到 host:port(host 支持 IP 或域名); 返回实际发送字节数, <=0 错误
        virtual int SendTo(SocketHandle sock, const void* data, size_t len,
            const std::string& host, uint16_t port) = 0;
        // 接收一个数据报; 返回实际字节数, <=0 错误/超时(超时错误见 GetSocketErrorTimeout)。
        // 不返回源地址: 载荷自带 AEAD 认证(PSK), 伪造源无法通过解密, 安全性由协议层保证。
        virtual int RecvFrom(SocketHandle sock, void* buf, size_t len) = 0;

        // ---------- 超时控制 ----------
        virtual bool SetSocketRecvTimeout(SocketHandle sock, int timeout_ms) = 0;
        // 同时设置收发超时(转发/长连接用, 防止对端不读导致 send 永久阻塞)
        virtual bool SetSocketTimeout(SocketHandle sock, int timeout_ms) = 0;

        // ---------- 字节序转换 ----------
        virtual uint32_t Hton32(uint32_t host_value) = 0;

        // ---------- 错误处理 ----------
        virtual int GetLastSocketError() = 0;
        virtual int GetSocketErrorTimeout() const = 0;
        // 判断 err 是否为阻塞 recv 的"良性瞬态"错误(与 Timeout 同属可重试):
        // Windows: ERROR_IO_PENDING(997, 超时取消竞态泄露的完成态, 健康链路
        // 实测偶发, 重试即正常) 与 WSAEWOULDBLOCK(10035)。注意 997 不是
        // WSAEWOULDBLOCK(10035), 首版修复曾混淆二者导致 997 判死(R6)。
        // Linux: EWOULDBLOCK(与 EAGAIN 同值)。
        virtual bool IsTransientRecvError(int err) const = 0;
    };

    // 工厂函数（由具体平台实现）
    std::unique_ptr<INetworkOps> CreateNetworkOps();

} // namespace cleat