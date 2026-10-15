#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "Core/Comms/Platform/IPipeOps.hpp"

namespace cleat {

    /**
     * @brief 管道帧编解码([4B 大端长度][body], 与 TCP 落线一致)
     *
     * PipeChannel 与 PipeSessionHub 共用; 提取自 L1 的 TryPopFrame/Write 头部
     * 拼装逻辑, 独立成纯函数便于两端语义一致。
     */
    namespace PipeFrame {

        inline constexpr size_t kHeaderBytes = 4;
        inline constexpr size_t kMaxFrameBytes = 4 * 1024 * 1024;

        // 在 wire 尾部追加一帧(4B 大端长度 + body)
        inline void AppendFrame(std::vector<uint8_t>& wire,
            const std::vector<uint8_t>& body) {
            const uint32_t n = static_cast<uint32_t>(body.size());
            wire.push_back(static_cast<uint8_t>(n >> 24));
            wire.push_back(static_cast<uint8_t>(n >> 16));
            wire.push_back(static_cast<uint8_t>(n >> 8));
            wire.push_back(static_cast<uint8_t>(n));
            wire.insert(wire.end(), body.begin(), body.end());
        }

        /**
         * @brief 从缓冲提取一帧; 成功时 out=body 并从 buf 移除已消费字节
         * @return 0=提取成功, 1=未收齐(继续等), -1=帧长超限(调用方应作废链路)
         */
        inline int TryExtractFrame(std::vector<uint8_t>& buf,
            std::vector<uint8_t>& out, size_t max_frame = kMaxFrameBytes) {
            if (buf.size() < kHeaderBytes) return 1;
            const size_t len = (static_cast<size_t>(buf[0]) << 24)
                | (static_cast<size_t>(buf[1]) << 16)
                | (static_cast<size_t>(buf[2]) << 8)
                | static_cast<size_t>(buf[3]);
            if (len > max_frame) return -1;
            if (buf.size() < kHeaderBytes + len) return 1;
            out.assign(buf.begin() + kHeaderBytes, buf.begin() + kHeaderBytes + len);
            buf.erase(buf.begin(), buf.begin() + kHeaderBytes + len);
            return 0;
        }

        // 整帧写出(循环补齐部分写); 返回 false = 管道错误/断开
        inline bool WriteAll(IPipeOps* ops, IPipeOps::PipeHandle handle,
            const std::vector<uint8_t>& body) {
            std::vector<uint8_t> wire;
            wire.reserve(kHeaderBytes + body.size());
            AppendFrame(wire, body);
            size_t sent = 0;
            while (sent < wire.size()) {
                const int w = ops->Write(handle, wire.data() + sent, wire.size() - sent);
                if (w <= 0) return false;
                sent += static_cast<size_t>(w);
            }
            return true;
        }

        // libsodium 全局初始化(幂等); randombytes 等前置要求
        void EnsureSodium();

        // 密钥派生: 任意长度口令 → 32B 会话密钥(SHA-256, libsodium)
        void DeriveKey(const std::string& secret, uint8_t out[32]);

        // HMAC-SHA512-256(libsodium crypto_auth): 32B 认证码
        void AuthCode(const uint8_t key[32], const uint8_t* msg, size_t len,
            uint8_t out[32]);

    } // namespace PipeFrame

} // namespace cleat
