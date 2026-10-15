/**
 * @file QuicWireCodec.hpp
 * @brief QUIC 会话线格式编解码(共享件, 平台无关)。
 * @details 会话载荷帧口径与 Tcp/Ws **完全同构**(WsWireCodec 会话段逐条照抄):
 *     [4B 大端长度][Encrypt(frame)][随机填充]
 *   差异仅在于无 HTTP 伪装壳 —— QUIC 自身的 TLS/UDP 形态即传输层伪装
 *   (与 WS"升级握手即壳"同理)。
 *   填充(TrafficShaper::AddPadding)与 server frame.go 同格式, 服务端复用同一 strip 逻辑。
 * 消费方: QuicTransport(会话载荷) / 夹具(环回对拍)。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace cleat {

    class QuicWireCodec {
    public:
        /// 会话载荷帧(明文)上限 —— 对齐 protocol.MaxBodySize(TCP 家族口径)
        static constexpr size_t kMaxSessionFrame = 50 * 1024 * 1024;

        /// 单帧线上长度硬上限(4B 头 + 密文扩展开销 + 填充上限 + 余量)。
        /// 供定界方在"长度头声明超过此值"时立即判非法, 防病态长度头拖住内存。
        static constexpr size_t kMaxWireFrame = kMaxSessionFrame + 4 * 1024;

        /// 明文帧 -> 线上字节: [4B 大端长度][Encrypt(frame)][随机填充]。
        /// false = 超上限或加密失败(不产出半成品)。
        static bool EncodeWire(const std::vector<uint8_t>& frame, std::vector<uint8_t>& out);

        /// 线上整帧(已按 4B 长度头切出, 含填充) -> 明文帧(原地还原)。
        /// false = 长度头不一致 / 填充非法 / 解密失败 —— 调用方按  熔断计数。
        static bool DecodeWire(std::vector<uint8_t>& wire);
    };

} // namespace cleat
