// WsWireCodec.cpp
#include "WsWireCodec.hpp"

#include "Common/Encoding/Base64.hpp"
#include "Common/Logger/ErrLog.hpp"
#include "Common/Logger/Logger.hpp"
#include "Core/Config/UA/UAProvider.hpp"
#include "Crypto/Engine/CryptoManager.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <random>
#include <iterator>

namespace cleat {

    namespace {

        // WS 握手 key 的随机源: mt19937 非加密需求(仅防碰撞/缓存污染),
        // 保密性由 TLS 与应用层加密承担 —— 与 PlatformWin_Http 的 UA 池同纪律。
        std::mt19937& Rng() {
            static std::mt19937 rng(static_cast<uint32_t>(
                std::random_device{}() ^
                static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count())));
            return rng;
        }

        inline uint32_t Rol32(uint32_t v, int b) {
            return (v << b) | (v >> (32 - b));
        }

        // 大端读取(buf 偏移 off 起 n 字节, n <= 8)
        uint64_t ReadBE(const std::vector<uint8_t>& buf, size_t off, size_t n) {
            uint64_t v = 0;
            for (size_t i = 0; i < n; ++i) {
                v = (v << 8) | buf[off + i];
            }
            return v;
        }

        // 行内大小写不敏感查找(握手头校验用; RFC 头名/值大小写不敏感)
        bool IContains(const std::string& hay, const std::string& needle) {
            if (needle.empty()) return true;
            if (hay.size() < needle.size()) return false;
            for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
                size_t j = 0;
                while (j < needle.size() &&
                    std::tolower(static_cast<unsigned char>(hay[i + j])) ==
                    std::tolower(static_cast<unsigned char>(needle[j]))) {
                    ++j;
                }
                if (j == needle.size()) return true;
            }
            return false;
        }

        // 握手响应头 UA: 统一 UAProvider( 组件化, 原文件私有副本收敛;
        // override=远程下发 http.user_agent 优先, 空=平台池随机)
        const std::string kUaPick = UAProvider::Pick();

        /// 帧填充上限(字节): [0, kMaxPad) —— 与 server frame.go / Tcp / QUIC **同格式**
        /// (帧尾 [随机填充][2B 小端填充长度], 4B 长度字段同步修正为 载荷+填充+2)。
        /// 本实现为 WS 模块自治件(不引用 Tcp 内部组件 —— 通道间不跨模块耦合,
        constexpr size_t kMaxPad = 512;

        /// 逐帧填充(同 Tcp/QUIC 的 AddPadding 行为; 非法帧<4B 原样返回)
        std::vector<uint8_t> AddFramePadding(const std::vector<uint8_t>& frame_with_length) {
            if (frame_with_length.size() < 4) return frame_with_length;
            static std::mutex rng_mutex;
            static std::mt19937 rng{ std::random_device{}() };
            std::lock_guard<std::mutex> lock(rng_mutex);
            const size_t pad_len = rng() % kMaxPad;

            std::vector<uint8_t> out;
            out.reserve(frame_with_length.size() + pad_len + 2);
            const uint32_t orig = static_cast<uint32_t>(ReadBE(frame_with_length, 0, 4));
            const uint32_t new_len = orig + static_cast<uint32_t>(pad_len) + 2u;
            out.push_back(static_cast<uint8_t>((new_len >> 24) & 0xFF));
            out.push_back(static_cast<uint8_t>((new_len >> 16) & 0xFF));
            out.push_back(static_cast<uint8_t>((new_len >> 8) & 0xFF));
            out.push_back(static_cast<uint8_t>(new_len & 0xFF));
            out.insert(out.end(), frame_with_length.begin() + 4, frame_with_length.end());
            for (size_t i = 0; i < pad_len; ++i) {
                out.push_back(static_cast<uint8_t>(rng() & 0xFF));
            }
            // 2B 填充长度(小端), 供接收端精确剥离
            out.push_back(static_cast<uint8_t>(pad_len & 0xFF));
            out.push_back(static_cast<uint8_t>((pad_len >> 8) & 0xFF));
            return out;
        }

        /// 剥离帧尾填充(原地); 帧体不足 2B 或填充长度非法 → false(丢弃该帧)
        bool StripFramePadding(std::vector<uint8_t>& frame_body) {
            if (frame_body.size() < 2) return false;
            const size_t pad_len = frame_body[frame_body.size() - 2]
                | (static_cast<size_t>(frame_body[frame_body.size() - 1]) << 8);
            if (pad_len + 2 > frame_body.size()) return false;
            frame_body.resize(frame_body.size() - pad_len - 2);
            return true;
        }

    } // namespace

    // ---------------------------------------------------------------------------
    // SHA1(RFC 3174)
    // ---------------------------------------------------------------------------

    void WsWireCodec::Sha1(const uint8_t* data, size_t len, uint8_t out[20]) {
        uint32_t h[5] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u };
        std::vector<uint8_t> msg(data, data + len);
        const uint64_t bits = static_cast<uint64_t>(len) * 8;
        msg.push_back(0x80);
        while (msg.size() % 64 != 56) {
            msg.push_back(0x00);
        }
        for (int i = 7; i >= 0; --i) {
            msg.push_back(static_cast<uint8_t>((bits >> (i * 8)) & 0xFF));
        }
        for (size_t off = 0; off < msg.size(); off += 64) {
            uint32_t w[80];
            for (int i = 0; i < 16; ++i) {
                const size_t p = off + static_cast<size_t>(i) * 4;
                w[i] = static_cast<uint32_t>(msg[p]) << 24
                    | static_cast<uint32_t>(msg[p + 1]) << 16
                    | static_cast<uint32_t>(msg[p + 2]) << 8
                    | static_cast<uint32_t>(msg[p + 3]);
            }
            for (int i = 16; i < 80; ++i) {
                w[i] = Rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
            }
            uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
            for (int i = 0; i < 80; ++i) {
                uint32_t f, k;
                if (i < 20) { f = (b & c) | ((~b) & d); k = 0x5A827999u; }
                else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
                else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
                else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
                const uint32_t t = Rol32(a, 5) + f + e + k + w[i];
                e = d; d = c; c = Rol32(b, 30); b = a; a = t;
            }
            h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
        }
        for (int i = 0; i < 5; ++i) {
            out[i * 4] = static_cast<uint8_t>(h[i] >> 24);
            out[i * 4 + 1] = static_cast<uint8_t>(h[i] >> 16);
            out[i * 4 + 2] = static_cast<uint8_t>(h[i] >> 8);
            out[i * 4 + 3] = static_cast<uint8_t>(h[i]);
        }
    }

    // ---------------------------------------------------------------------------
    // 握手
    // ---------------------------------------------------------------------------

    std::string WsWireCodec::MakeClientKey() {
        std::vector<uint8_t> raw(16);
        for (auto& b : raw) {
            b = static_cast<uint8_t>(Rng()());
        }
        return B64Encode(raw.data(), raw.size());
    }

    std::string WsWireCodec::ExpectedAccept(const std::string& client_key) {
        static const char kGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";   // RFC6455
        std::string cat = client_key + kGuid;
        uint8_t digest[20];
        Sha1(reinterpret_cast<const uint8_t*>(cat.data()), cat.size(), digest);
        return B64Encode(digest, sizeof(digest));
    }

    std::string WsWireCodec::BuildHandshakeRequest(const std::string& host, uint16_t port,
        const std::string& path, const std::string& client_key) {
        // UA: 浏览器形状随机池(伪装面; 与 HTTPS 通道 UA 口径一致)
        const std::string ua = kUaPick;
        // Host 头: 默认端口 443 不带端口(浏览器形状)
        std::string host_header = host;
        if (port != 443 && port != 0) {
            host_header += ":" + std::to_string(port);
        }
        std::string req;
        req.reserve(256);
        req += "GET " + (path.empty() ? std::string("/") : path) + " HTTP/1.1\r\n";
        req += "Host: " + host_header + "\r\n";
        req += "Upgrade: websocket\r\n";
        req += "Connection: Upgrade\r\n";
        req += "Sec-WebSocket-Key: " + client_key + "\r\n";
        req += "Sec-WebSocket-Version: 13\r\n";
        req += "User-Agent: ";
        req += ua;
        req += "\r\n\r\n";
        return req;
    }

    bool WsWireCodec::ValidateUpgradeResponse(const std::string& response_head,
        const std::string& client_key) {
        // 状态行必须 101(RFC6455 唯一成功码)
        const size_t line_end = response_head.find("\r\n");
        if (line_end == std::string::npos) return false;
        {
            const std::string status_line = response_head.substr(0, line_end);
            // "HTTP/1.1 101 Switching Protocols": 取第二段数字判定, 不做全文匹配
            const size_t sp1 = status_line.find(' ');
            if (sp1 == std::string::npos) return false;
            const size_t sp2 = status_line.find(' ', sp1 + 1);
            if (sp2 == std::string::npos) return false;
            if (status_line.compare(sp1 + 1, sp2 - sp1 - 1, "101") != 0) return false;
        }
        // 头逐行解析(容忍大小写与空白), 目标三头
        bool has_upgrade = false, has_connection = false, has_accept = false;
        const std::string expected = ExpectedAccept(client_key);
        size_t pos = line_end + 2;
        while (pos < response_head.size()) {
            const size_t eol = response_head.find("\r\n", pos);
            const std::string line = (eol == std::string::npos)
                ? response_head.substr(pos) : response_head.substr(pos, eol - pos);
            if (line.empty()) break;
            const size_t colon = line.find(':');
            if (colon != std::string::npos) {
                const std::string name = line.substr(0, colon);
                size_t vs = colon + 1;
                while (vs < line.size() && line[vs] == ' ') ++vs;
                const std::string value = line.substr(vs);
                if (IContains(name, "upgrade") && IContains(value, "websocket")) {
                    has_upgrade = true;
                }
                if (IContains(name, "connection") && IContains(value, "upgrade")) {
                    has_connection = true;
                }
                if (IContains(name, "sec-websocket-accept")) {
                    // 值精确比对(基串无空白; 尾部 \r 已被分行剥离)
                    std::string v = value;
                    while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
                    if (v == expected) has_accept = true;
                }
            }
            if (eol == std::string::npos) break;
            pos = eol + 2;
        }
        return has_upgrade && has_connection && has_accept;
    }

    // ---------------------------------------------------------------------------
    // 帧
    // ---------------------------------------------------------------------------

    std::vector<uint8_t> WsWireCodec::MakeMask() {
        std::vector<uint8_t> mask(4);
        for (auto& b : mask) {
            b = static_cast<uint8_t>(Rng()());
        }
        return mask;
    }

    bool WsWireCodec::EncodeClientFrame(uint8_t opcode, const uint8_t* data, size_t len,
        std::vector<uint8_t>& out) {
        if (len > kMaxFramePayload) {
            return false;
        }
        out.clear();
        out.reserve(14 + len);
        out.push_back(static_cast<uint8_t>(0x80 | (opcode & 0x0F)));   // FIN=1, 本端单帧整发
        const std::vector<uint8_t> mask = MakeMask();
        if (len < 126) {
            out.push_back(static_cast<uint8_t>(0x80 | len));           // MASK=1
        }
        else if (len <= 0xFFFF) {
            out.push_back(0x80 | 126);
            out.push_back(static_cast<uint8_t>(len >> 8));
            out.push_back(static_cast<uint8_t>(len));
        }
        else {
            out.push_back(0x80 | 127);
            for (int i = 7; i >= 0; --i) {
                out.push_back(static_cast<uint8_t>((static_cast<uint64_t>(len) >> (i * 8)) & 0xFF));
            }
        }
        out.insert(out.end(), mask.begin(), mask.end());
        for (size_t i = 0; i < len; ++i) {
            out.push_back(data[i] ^ mask[i % 4]);
        }
        return true;
    }

    int WsWireCodec::TryParseFrame(std::vector<uint8_t>& buf, WsFrame& out) {
        if (buf.size() < 2) return 0;
        if (buf[0] & 0x70) return -1;   // RSV 位必须为 0(未协商扩展)
        const bool fin = (buf[0] & 0x80) != 0;
        const uint8_t opcode = static_cast<uint8_t>(buf[0] & 0x0F);
        const bool masked = (buf[1] & 0x80) != 0;
        uint64_t len = buf[1] & 0x7F;
        size_t hdr = 2;
        if (len == 126) {
            if (buf.size() < hdr + 2) return 0;
            len = ReadBE(buf, hdr, 2);
            hdr += 2;
        }
        else if (len == 127) {
            if (buf.size() < hdr + 8) return 0;
            len = ReadBE(buf, hdr, 8);
            hdr += 8;
        }
        uint8_t mask[4] = { 0, 0, 0, 0 };
        if (masked) {
            if (buf.size() < hdr + 4) return 0;
            for (int i = 0; i < 4; ++i) mask[i] = buf[hdr + i];
            hdr += 4;
        }
        if (len > kMaxFramePayload) return -1;   // 长度头谎报: 按协议错误
        if (buf.size() < hdr + len) return 0;    // 未完整: 缓冲原状
        out.opcode = opcode;
        out.fin = fin;
        out.payload.assign(buf.begin() + static_cast<std::ptrdiff_t>(hdr),
            buf.begin() + static_cast<std::ptrdiff_t>(hdr + len));
        if (masked) {
            for (size_t i = 0; i < len; ++i) {
                out.payload[i] ^= mask[i % 4];
            }
        }
        buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(hdr + len));
        return 1;
    }

    // ---------------------------------------------------------------------------
    // ---------------------------------------------------------------------------

    bool WsWireCodec::EncodeSessionWire(const std::vector<uint8_t>& frame,
        std::vector<uint8_t>& out) {
        if (frame.size() > kMaxSessionFrame) {
            CLEAT_LOGE(ErrDom::Net, ErrCode::ResourceExhausted,
                "WsWireCodec: 会话帧超上限 len=" << frame.size());
            return false;
        }
        std::vector<uint8_t> wire = frame;
        if (CryptoManager::Instance().IsEnabled()) {
            std::vector<uint8_t> enc;
            if (!CryptoManager::Instance().Encrypt(frame, enc)) {
                CLEAT_LOGE(ErrDom::Net, ErrCode::CryptoOpFailed,
                    "WsWireCodec: 帧加密失败 (len=" << frame.size() << ")");
                return false;
            }
            wire = std::move(enc);
        }
        // [4B 大端长度][载荷], 与 TcpWireCodec 同构
        std::vector<uint8_t> framed;
        framed.reserve(4 + wire.size());
        const uint32_t len = static_cast<uint32_t>(wire.size());
        framed.push_back(static_cast<uint8_t>(len >> 24));
        framed.push_back(static_cast<uint8_t>(len >> 16));
        framed.push_back(static_cast<uint8_t>(len >> 8));
        framed.push_back(static_cast<uint8_t>(len));
        framed.insert(framed.end(), wire.begin(), wire.end());
        // AddFramePadding 会同步修正 4B 长度); 无 HTTP 伪装壳 —— WS 升级握手即壳。
        out = AddFramePadding(framed);
        return true;
    }

    bool WsWireCodec::DecodeSessionBody(std::vector<uint8_t>& msg) {
        if (msg.size() < 4) {
            CLEAT_LOGE(ErrDom::Net, ErrCode::IntegrityMismatch,
                "WsWireCodec: 消息短于 4B 长度头 len=" << msg.size());
            return false;
        }
        const uint32_t declared = static_cast<uint32_t>(ReadBE(msg, 0, 4));
        if (declared != msg.size() - 4) {
            CLEAT_LOGE(ErrDom::Net, ErrCode::IntegrityMismatch,
                "WsWireCodec: 长度头不一致 declared=" << declared
                << " actual=" << (msg.size() - 4));
            return false;
        }
        std::vector<uint8_t> body(msg.begin() + 4, msg.end());
        // 剥填充(密文层, 与 server stripFramePadding 同格式)
        if (!StripFramePadding(body)) {
            CLEAT_LOGE(ErrDom::Net, ErrCode::IntegrityMismatch,
                "WsWireCodec: 帧填充剥离失败 (len=" << body.size() << ")");
            return false;
        }
        if (CryptoManager::Instance().IsEnabled()) {
            std::vector<uint8_t> dec;
            if (!CryptoManager::Instance().Decrypt(body, dec)) {
                CLEAT_LOGE(ErrDom::Net, ErrCode::CryptoOpFailed,
                    "WsWireCodec: 帧解密失败 (len=" << body.size() << "), 可能 PSK 不匹配或数据被篡改");
                return false;
            }
            msg = std::move(dec);
        }
        else {
            msg = std::move(body);
        }
        return true;
    }

} // namespace cleat
