#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// 前置声明
namespace cleat {
    class RingBuffer;
}

namespace cleat {

    /**
     * @brief 从 RingBuffer 中解析 TCP 帧（定长头 + 变长体）
     *
     * 帧格式： [4字节长度头（网络序，uint32_t）] + [包体（二进制数据）]
     * - 长度头仅表示包体长度，不包含自身。
     * - 最大包体大小硬上限为 50 MiB。
     * - 线程安全（依赖 RingBuffer 的内部锁）。
     * - 所有操作均为非阻塞，数据不足时静默返回 false。
     */
    class FrameParser {
    public:
        /**
         * @param ring 外部环形缓冲区引用，生命周期必须长于 FrameParser
         */
        explicit FrameParser(RingBuffer& ring);

        // 禁止拷贝与移动（引用成员不可移动）
        FrameParser(const FrameParser&) = delete;
        FrameParser& operator=(const FrameParser&) = delete;
        FrameParser(FrameParser&&) = delete;
        FrameParser& operator=(FrameParser&&) = delete;

        ~FrameParser() = default;

        /**
         * @brief 尝试从 RingBuffer 中提取一个完整帧
         * @param out_payload 输出参数，存放包体数据（不含长度头）
         * @return 成功提取完整帧返回 true，否则返回 false
         * @note 若成功，out_payload 会被填充；若失败，out_payload 保持不变。
         *       若长度头非法（<=0 或 > MAX_PACKET_SIZE），会丢弃该帧（长度头及已到达的包体数据），返回 false。
         */
        bool TryParse(std::vector<uint8_t>& out_payload);

        /**
         * @brief 最大允许包体大小（49 MiB）
         */
        static constexpr size_t MAX_PACKET_SIZE = 49 * 1024 * 1024;

    private:
        RingBuffer& ring_;

        // 批量丢弃非法帧数据的块大小（4 KiB）
        static constexpr size_t CHUNK_SIZE = 4 * 1024;

        // 辅助：从缓冲区读取网络序(大端)的 uint32_t 长度头, 返回主机序。若不足4字节，返回 false。
        bool peek_length(uint32_t& out_len) const;
    };

} // namespace cleat