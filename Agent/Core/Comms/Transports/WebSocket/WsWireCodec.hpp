#pragma once
// WebSocket 线格式编解码(共享件, 平台无关):
//   1) RFC6455 握手: 客户端 key 生成 / Accept 校验(SHA1+base64 自带实现 —— Sodium 无 SHA1)
//   2) RFC6455 帧: 客户端出站帧(必掩码,)编码 + 流式解析(服务端入站帧不掩码)
//      同构但**不含** HTTP 伪装壳: WS 升级握手本身即伪装壳; 填充保留(TLS 不藏长度)。
// 消费方: PlatformLinux_Wss(握手+帧循环) / 夹具(环回对拍) / WsTransport(会话载荷)。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cleat {

    /// 解析出的单个 WS 帧(RFC6455)
    struct WsFrame {
        uint8_t opcode = 0;
        bool fin = false;
        std::vector<uint8_t> payload;
    };

    class WsWireCodec {
    public:
        // ---- RFC6455 帧操作码() ----
        static constexpr uint8_t kOpContinuation = 0x0;
        static constexpr uint8_t kOpText = 0x1;
        static constexpr uint8_t kOpBinary = 0x2;
        static constexpr uint8_t kOpClose = 0x8;
        static constexpr uint8_t kOpPing = 0x9;
        static constexpr uint8_t kOpPong = 0xA;

        /// 单帧载荷硬上限(防病态长度头; 会话帧上限见 kMaxSessionWire)
        static constexpr size_t kMaxFramePayload = 64 * 1024 * 1024;
        /// 会话载荷帧(明文)上限 —— 对齐 protocol.MaxBodySize(TCP 家族口径)
        static constexpr size_t kMaxSessionFrame = 50 * 1024 * 1024;

        // ---- SHA1(RFC 3174; 共享实现, Sodium 不提供 SHA1) ----
        static void Sha1(const uint8_t* data, size_t len, uint8_t out[20]);

        // ---- 握手(客户端侧) ----
        /// 16B 随机 key -> base64(; 24 字符)
        static std::string MakeClientKey();
        /// Accept 期望值 = base64(SHA1(key + 固定 GUID))()
        static std::string ExpectedAccept(const std::string& client_key);
        /// 客户端握手请求文本(GET + Upgrade 头族; UA 取浏览器形状)
        static std::string BuildHandshakeRequest(const std::string& host, uint16_t port,
            const std::string& path, const std::string& client_key);
        /**
         * @brief 校验服务端握手响应
         * @return true = 101 + Upgrade: websocket + Connection: Upgrade + Accept 匹配;
         *         false 的唯一语义 = 响应非法(握手失败)
         */
        static bool ValidateUpgradeResponse(const std::string& response_head,
            const std::string& client_key);

        // ---- 帧(本端是客户端: 出站必掩码, 入站必不掩码) ----
        /// 编码一帧(整帧整发; 掩码每帧随机)
        static bool EncodeClientFrame(uint8_t opcode, const uint8_t* data, size_t len,
            std::vector<uint8_t>& out);
        /**
         * @brief 从流缓冲解析一帧(成功即消费已解析字节)
         * @return 1 = out 有效; 0 = 数据不足(缓冲保持原状); -1 = 协议错误(缓冲不作承诺)
         */
        static int TryParseFrame(std::vector<uint8_t>& buf, WsFrame& out);

        /// 明文会话帧 -> 线上字节: [4B 大端长度][Encrypt(frame)][随机填充](整条 = 一个 WS 消息)。
        /// false = 超上限或加密失败(不产出半成品)。
        static bool EncodeSessionWire(const std::vector<uint8_t>& frame,
            std::vector<uint8_t>& out);
        /// 线上字节(WS 消息体) -> 明文会话帧(原地还原)。
        /// false = 长度头不一致 / 填充非法 / 解密失败 —— 调用方按  熔断计数。
        static bool DecodeSessionBody(std::vector<uint8_t>& msg);

    private:
        static std::vector<uint8_t> MakeMask();
    };

} // namespace cleat
