#include "TrafficShaper.hpp"
#include "Core/Comms/Transports/Tcp/Internal/RingBuffer.hpp"
#include "Core/Config/UA/UAProvider.hpp"

#include <random>
#include<mutex>
namespace cleat {

    namespace {
        // 候选路径池: 模拟常见 Web 请求形态(后续可做成配置/更大池)
        const char* kFakePaths[] = {
            "/index.php",
            "/api/v1/status",
            "/assets/app.js",
            "/favicon.ico",
        };
        // 帧填充上限(字节): 每帧在密文后追加 [0, kMaxPad) 随机填充 + 2B 填充长度。
        // 必须与 Server frame.go 的 maxFramePadding 一致, 否则两端无法互剥。
        constexpr size_t kMaxPad = 512;
        static std::mutex g_rng_mutex;
    }

    std::vector<uint8_t> TrafficShaper::Wrap(MimicDirection dir,
        const std::vector<uint8_t>& frame_with_length, const std::string& ua) {
        // 帧填充: 载荷尾追加随机填充 + 2B 填充长度, 并修正 4B 长度字段
        const std::vector<uint8_t> padded = AddPadding(frame_with_length);
        std::vector<uint8_t> header = (dir == MimicDirection::Request)
            ? BuildRequestHeader(ua)
            : BuildResponseHeader(padded.size());
        std::vector<uint8_t> out;
        out.reserve(header.size() + padded.size());
        out.insert(out.end(), header.begin(), header.end());
        out.insert(out.end(), padded.begin(), padded.end());
        return out;
    }

    std::vector<uint8_t> TrafficShaper::AddPadding(
        const std::vector<uint8_t>& frame_with_length) {
        // 非法帧(不足 4B 长度头)原样返回, 不填充
        if (frame_with_length.size() < 4) return frame_with_length;
        std::lock_guard<std::mutex>lock(g_rng_mutex);
        static std::mt19937 rng{ std::random_device{}() };
        const size_t pad_len = rng() % kMaxPad; // [0, kMaxPad)

        std::vector<uint8_t> out;
        out.reserve(frame_with_length.size() + pad_len + 2);

        // 修正 4B 大端长度字段: 原长度 + 填充 + 2B 填充长度
        uint32_t orig = (static_cast<uint32_t>(frame_with_length[0]) << 24)
            | (static_cast<uint32_t>(frame_with_length[1]) << 16)
            | (static_cast<uint32_t>(frame_with_length[2]) << 8)
            | frame_with_length[3];
        const uint32_t new_len = orig + static_cast<uint32_t>(pad_len) + 2u;
        out.push_back(static_cast<uint8_t>((new_len >> 24) & 0xFF));
        out.push_back(static_cast<uint8_t>((new_len >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((new_len >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(new_len & 0xFF));

        // 原载荷
        out.insert(out.end(), frame_with_length.begin() + 4, frame_with_length.end());

        // 随机填充字节(内容无意义, 仅打散帧长分布)
        for (size_t i = 0; i < pad_len; ++i) {
            out.push_back(static_cast<uint8_t>(rng() & 0xFF));
        }

        // 2B 填充长度(小端), 供接收端精确剥离
        out.push_back(static_cast<uint8_t>(pad_len & 0xFF));
        out.push_back(static_cast<uint8_t>((pad_len >> 8) & 0xFF));
        return out;
    }

    bool TrafficShaper::StripPadding(std::vector<uint8_t>& frame_body) {
        if (frame_body.size() < 2) return false;
        const size_t pad_len = frame_body[frame_body.size() - 2]
            | (static_cast<size_t>(frame_body[frame_body.size() - 1]) << 8);
        if (pad_len + 2 > frame_body.size()) return false;
        frame_body.resize(frame_body.size() - pad_len - 2);
        return true;
    }

    std::vector<uint8_t> TrafficShaper::BuildRequestHeader(const std::string& ua) {
        std::lock_guard<std::mutex>lock(g_rng_mutex);
        static std::mt19937 rng{ std::random_device{}() };
        const char* path = kFakePaths[rng() % (sizeof(kFakePaths) / sizeof(kFakePaths[0]))];
        // UA: 连接级粘性(TcpTransport 连接时经 UAProvider::Pick 选定并传入);
        // 空(兼容旧调用/测试) = 统一池随机 —— 池收敛于 UAProvider, 本文件不再持副本。
        const std::string effective_ua = ua.empty() ? UAProvider::Pick() : ua;

        std::string h = "GET ";
        h += path;
        h += " HTTP/1.1\r\n";
        h += "Host: update.microsoft.com\r\n";
        h += "User-Agent: ";
        h += effective_ua;
        h += "\r\n";
        h += "Accept: */*\r\n";
        h += "Connection: keep-alive\r\n";
        h += "\r\n";
        return std::vector<uint8_t>(h.begin(), h.end());
    }

    std::vector<uint8_t> TrafficShaper::BuildResponseHeader(size_t body_len) {
        std::string h = "HTTP/1.1 200 OK\r\n";
        h += "Content-Type: application/octet-stream\r\n";
        h += "Content-Length: " + std::to_string(body_len) + "\r\n";
        h += "Cache-Control: no-cache\r\n";
        h += "\r\n";
        return std::vector<uint8_t>(h.begin(), h.end());
    }

    bool TrafficShaper::TryStripHeader(RingBuffer& ring) {
        constexpr size_t kMaxHeader = 4096;
        uint8_t buf[kMaxHeader];
        const size_t n = ring.Peek(buf, kMaxHeader);
        if (n == 0) return false;

        for (size_t i = 0; i + 3 < n; ++i) {
            if (buf[i] == '\r' && buf[i + 1] == '\n' &&
                buf[i + 2] == '\r' && buf[i + 3] == '\n') {
                const size_t consume = i + 4;
                size_t left = consume;
                uint8_t tmp[512];
                while (left > 0) {
                    const size_t take = ring.Read(tmp, left < sizeof(tmp) ? left : sizeof(tmp));
                    if (take == 0) return false; // Peek 已确认可读, 理论不可达
                    left -= take;
                }
                return true;
            }
        }

        if (n == kMaxHeader) {
            // 4KB 内未找到分隔符: 头异常或超长, 丢弃已读部分继续找, 防恶意头卡死
            uint8_t tmp[512];
            size_t left = n;
            while (left > 0) {
                const size_t take = ring.Read(tmp, left < sizeof(tmp) ? left : sizeof(tmp));
                if (take == 0) break;
                left -= take;
            }
            return false; // 下一轮继续
        }
        return false; // 数据不足, 等待更多
    }

} // namespace cleat
