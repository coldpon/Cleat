/**
 * @file QuicWireCodec.cpp
 * @brief QUIC 会话线格式实现(口径与 WsWireCodec 会话段逐条一致)
 */
#include "Core/Comms/Transports/Quic/QuicWireCodec.hpp"

#include "Common/Logger/ErrLog.hpp"
#include "Common/Logger/Logger.hpp"
#include "Crypto/Engine/CryptoManager.hpp"

#include <mutex>
#include <random>

namespace cleat {

    namespace {

        /// 帧填充上限(字节): [0, kMaxPad) —— 与 server frame.go / Tcp / Ws **同格式**
        /// (帧尾 [随机填充][2B 小端填充长度], 4B 长度字段同步修正为 载荷+填充+2)。
        /// 本实现为 QUIC 模块自治件(不引用 Tcp 内部组件 —— 通道间不跨模块耦合,
        constexpr size_t kMaxPad = 512;

        /// 大端读 4B(uint32)
        uint32_t ReadBe32(const uint8_t* p) {
            return (static_cast<uint32_t>(p[0]) << 24)
                | (static_cast<uint32_t>(p[1]) << 16)
                | (static_cast<uint32_t>(p[2]) << 8)
                | static_cast<uint32_t>(p[3]);
        }

        /// 逐帧填充: 追加 [随机填充][2B 小端填充长度], 并修正 4B 长度字段。
        /// (同 Tcp/Ws 的 AddPadding 行为; 非法帧<4B 原样返回)
        std::vector<uint8_t> AddFramePadding(const std::vector<uint8_t>& frame_with_length) {
            if (frame_with_length.size() < 4) return frame_with_length;
            static std::mutex rng_mutex;
            static std::mt19937 rng{ std::random_device{}() };
            std::lock_guard<std::mutex> lock(rng_mutex);
            const size_t pad_len = rng() % kMaxPad;

            std::vector<uint8_t> out;
            out.reserve(frame_with_length.size() + pad_len + 2);
            const uint32_t orig = ReadBe32(frame_with_length.data());
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

    bool QuicWireCodec::EncodeWire(const std::vector<uint8_t>& frame,
        std::vector<uint8_t>& out) {
        if (frame.size() > kMaxSessionFrame) {
            CLEAT_LOGE(ErrDom::Net, ErrCode::ResourceExhausted,
                "QuicWireCodec: 会话帧超上限 len=" << frame.size());
            return false;
        }
        std::vector<uint8_t> wire = frame;
        if (CryptoManager::Instance().IsEnabled()) {
            // 双层加密的外层(TLS 由 QUIC 传输承担): 与 TCP/Ws 同纪律, 握手帧的
            // 明文旁路由 CryptoManager 内部处理(调用方无需特判)。
            std::vector<uint8_t> enc;
            if (!CryptoManager::Instance().Encrypt(frame, enc)) {
                CLEAT_LOGE(ErrDom::Net, ErrCode::CryptoOpFailed,
                    "QuicWireCodec: 帧加密失败 (len=" << frame.size() << ")");
                return false;
            }
            wire = std::move(enc);
        }
        // [4B 大端长度][载荷], 与 Tcp/Ws 同构
        std::vector<uint8_t> framed;
        framed.reserve(4 + wire.size());
        const uint32_t len = static_cast<uint32_t>(wire.size());
        framed.push_back(static_cast<uint8_t>(len >> 24));
        framed.push_back(static_cast<uint8_t>(len >> 16));
        framed.push_back(static_cast<uint8_t>(len >> 8));
        framed.push_back(static_cast<uint8_t>(len));
        framed.insert(framed.end(), wire.begin(), wire.end());
        // 密文后追加随机填充(与 server frame.go / Tcp / Ws 同格式, 会同步修正
        // 4B 长度); 无 HTTP 伪装壳 —— QUIC 自身形态即壳。
        out = AddFramePadding(framed);
        return true;
    }

    bool QuicWireCodec::DecodeWire(std::vector<uint8_t>& wire) {
        if (wire.size() < 4) {
            CLEAT_LOGE(ErrDom::Net, ErrCode::IntegrityMismatch,
                "QuicWireCodec: 帧短于 4B 长度头 len=" << wire.size());
            return false;
        }
        // 长度头与帧体一致性(字节流定界下调用方按头切帧, 此处应恒等; 不等=协议垃圾)
        const uint32_t declared = ReadBe32(wire.data());
        if (declared != wire.size() - 4) {
            CLEAT_LOGE(ErrDom::Net, ErrCode::IntegrityMismatch,
                "QuicWireCodec: 长度头不一致 declared=" << declared
                << " actual=" << (wire.size() - 4));
            return false;
        }
        std::vector<uint8_t> body(wire.begin() + 4, wire.end());
        // 剥填充(密文层, 与 server stripFramePadding 同格式)
        if (!StripFramePadding(body)) {
            CLEAT_LOGE(ErrDom::Net, ErrCode::IntegrityMismatch,
                "QuicWireCodec: 帧填充剥离失败 (len=" << body.size() << ")");
            return false;
        }
        if (CryptoManager::Instance().IsEnabled()) {
            std::vector<uint8_t> dec;
            if (!CryptoManager::Instance().Decrypt(body, dec)) {
                CLEAT_LOGE(ErrDom::Net, ErrCode::CryptoOpFailed,
                    "QuicWireCodec: 帧解密失败 (len=" << body.size()
                    << "), 可能 PSK 不匹配或数据被篡改");
                return false;
            }
            wire = std::move(dec);
        }
        else {
            wire = std::move(body);
        }
        return true;
    }

} // namespace cleat
