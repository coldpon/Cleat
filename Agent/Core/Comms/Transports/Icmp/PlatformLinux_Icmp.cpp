// PlatformLinux_Icmp.cpp
// Linux ICMP 回声传输: ping socket(SOCK_DGRAM + IPPROTO_ICMP, 免 raw socket)。
// - 免 root 前提: 系统允许 ping group 使用 ICMP 回显(常见发行版默认允许,
//   受限系统见 /proc/sys/net/ipv4/ping_group_range; 失败时日志明示)
// - 语义与 Windows(IcmpSendEcho) 对齐: 发 echo 请求(payload=上行分片),
//   等应答并把应答 data 区(= 下行分片)填入 reply_payload
// - 仅应答 echo reply(type=0) 且 id/seq 匹配本进程本 socket
#include "Core/Comms/Transports/Icmp/IcmpOps.hpp"
#include "Core/Comms/Platform/PlatformLinux_Network.hpp"
#include "Common/Logger/ErrLog.hpp"
#include "Common/Logger/Logger.hpp"

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip_icmp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <poll.h>

namespace cleat {

namespace {

// ICMP echo header(8 字节, 与 netinet/ip_icmp.h 的 icmphdr 布局一致;
// 显式定义避免平台头差异)
#pragma pack(push, 1)
struct IcmpEchoHeader {
    uint8_t type;      // 8=echo request, 0=echo reply
    uint8_t code;      // 0
    uint16_t checksum;
    uint16_t id;       // 本进程标识(匹配应答)
    uint16_t seq;      // 递增序号(匹配应答)
};
#pragma pack(pop)

constexpr uint8_t kIcmpEchoRequest = 8;
constexpr uint8_t kIcmpEchoReply = 0;

uint16_t IcmpChecksum(const uint8_t* data, size_t len) {
    uint32_t sum = 0;
    size_t i = 0;
    for (; i + 1 < len; i += 2) {
        sum += (static_cast<uint16_t>(data[i]) << 8) | data[i + 1];
    }
    if (i < len) {
        sum += static_cast<uint16_t>(data[i]) << 8;   // 奇数尾字节
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return static_cast<uint16_t>(~sum);
}

} // namespace

class IcmpOpsLinux : public IIcmpOps {
public:
    IcmpOpsLinux() = default;
    ~IcmpOpsLinux() override { Reset(); }

    bool Echo(const std::string& target, const std::vector<uint8_t>& payload,
        std::vector<uint8_t>& reply_payload, int timeout_ms) override {
        reply_payload.clear();

        // 目标仅支持 IPv4(与 Win 侧 inet_addr 同约束); 校验+转换一次完成
        sockaddr_in peer{};
        peer.sin_family = AF_INET;
        if (inet_pton(AF_INET, target.c_str(), &peer.sin_addr) != 1) {
            CLEAT_LOGE(ErrDom::Net, ErrCode::InvalidParam, "IcmpOpsLinux: 目标非 IPv4: " << target);
            return false;
        }

        if (!EnsureSocket()) {
            return false;
        }

        // ---- 组 echo 请求 ----
        const uint16_t id = static_cast<uint16_t>(getpid() & 0xFFFF);
        const uint16_t seq = seq_++;
        std::vector<uint8_t> packet(sizeof(IcmpEchoHeader) + payload.size());
        auto* hdr = reinterpret_cast<IcmpEchoHeader*>(packet.data());
        hdr->type = kIcmpEchoRequest;
        hdr->code = 0;
        hdr->id = htons(id);
        hdr->seq = htons(seq);
        if (!payload.empty()) {
            std::memcpy(packet.data() + sizeof(IcmpEchoHeader),
                payload.data(), payload.size());
        }
        hdr->checksum = 0;
        hdr->checksum = htons(IcmpChecksum(packet.data(), packet.size()));

        // ---- 发送 ----
        if (sendto(sock_, packet.data(), packet.size(), 0,
            reinterpret_cast<const sockaddr*>(&peer), sizeof(peer)) < 0) {
            LOG_WARN("IcmpOpsLinux: sendto 失败 errno=" << errno << " (" << std::strerror(errno) << ")");
            return false;
        }

        // ---- 等待应答(id/seq 匹配, 丢弃杂包) ----
        const int timeout = (timeout_ms > 0) ? timeout_ms : 3000;
        if (poll(&pollfd_, 1, timeout) <= 0) {
            return false;   // 超时/被信号打断, 按失败处理
        }

        std::vector<uint8_t> recvbuf(sizeof(IcmpEchoHeader) + 1500);
        for (int attempt = 0; attempt < 4; ++attempt) {
            sockaddr_in from{};
            socklen_t fromlen = sizeof(from);
            const ssize_t n = recvfrom(sock_, recvbuf.data(), recvbuf.size(), 0,
                reinterpret_cast<sockaddr*>(&from), &fromlen);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }
            // ping socket 收到的从 ICMP 头开始(无 IP 头)
            if (static_cast<size_t>(n) < sizeof(IcmpEchoHeader)) {
                continue;
            }
            const auto* rh = reinterpret_cast<const IcmpEchoHeader*>(recvbuf.data());
            if (rh->type != kIcmpEchoReply || ntohs(rh->id) != id
                || ntohs(rh->seq) != seq) {
                continue;   // 杂包(其他进程/其他序号), 丢弃继续等
            }
            const size_t data_len = static_cast<size_t>(n) - sizeof(IcmpEchoHeader);
            reply_payload.assign(recvbuf.data() + sizeof(IcmpEchoHeader),
                recvbuf.data() + sizeof(IcmpEchoHeader) + data_len);
            return true;
        }
        return false;
    }

    void Reset() override {
        if (sock_ >= 0) {
            ::close(sock_);
            sock_ = -1;
            pollfd_.fd = -1;
        }
    }

private:
    bool EnsureSocket() {
        if (sock_ >= 0) return true;
        sock_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
        if (sock_ < 0) {
            CLEAT_LOGE(ErrDom::Net, ErrCode::PlatformApiFailed, "IcmpOpsLinux: 创建 ping socket 失败 errno=" << errno
                << " (" << std::strerror(errno)
                << ") — 需要 root 或系统允许 ping group(/proc/sys/net/ipv4/ping_group_range)");
            return false;
        }
        pollfd_.fd = sock_;
        pollfd_.events = POLLIN;
        return true;
    }

private:
    int sock_ = -1;
    uint16_t seq_ = 0;
    pollfd pollfd_{};
};

std::unique_ptr<IIcmpOps> CreateIcmpOps() {
    return std::make_unique<IcmpOpsLinux>();
}

} // namespace cleat
