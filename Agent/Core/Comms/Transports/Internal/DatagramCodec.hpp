/**
 * @file DatagramCodec.hpp
 * @brief 数据报分片/重组编解码器（Phase 3，DNS/ICMP 通道用）
 * @details
 * 大帧（加密后）切成小分片，每片作为一个“查询/回声”交互传输；
 * 接收侧按报序号重组。分片头格式：
 * [1B magic=0xA7][2B 报序号 小端][1B 片序号][1B 总片数][2B 片长 小端] + 片数据
 * 。
 * 重组按报序号聚合分片，按片序号排序，收齐总片数后拼接；
 * 超时（30s）回收防泄漏。
 * 线程安全：由 DnsTransport/IcmpTransport 单线程使用，无锁。
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace cleat {

    /**
     * @brief 数据报编解码器（静态工具类）
     * @details 提供分片（Fragment）和头部解析（ParseHeader）功能，
     *          内置重组器（Reassembler）用于收片拼接。
     */
    class DatagramCodec {
    public:
        static constexpr uint8_t  kMagic = 0xA7;                     ///< 分片头魔数
        static constexpr size_t   kHeaderSize = 7;                   ///< 分片头固定长度（字节）
        static constexpr uint32_t kReassemblyTimeoutMs = 30000;      ///< 重组超时（毫秒）
        static constexpr size_t   kMaxPendingEntries = 128;
        static constexpr size_t   kMaxPendingBytes = 4 * 1024 * 1024;

        /**
         * @brief 将数据分片为多个数据报分片
         * @param data           待分片的原始数据
         * @param max_frag_payload 每片最大负载长度（字节）
         * @return 分片列表，每个分片包含完整头部 + 负载
         * @details
         * 分片序号从 0 开始递增，回绕安全（16 位短窗口）。
         * 即使数据小于等于 max_frag_payload，也生成一个带完整头的分片。
         */
        static std::vector<std::vector<uint8_t>> Fragment(
            const std::vector<uint8_t>& data, size_t max_frag_payload);

        /**
         * @brief 解析分片头部
         * @param chunk       原始分片数据（含头部 + 负载）
         * @param[out] seq       报序号（16 位）
         * @param[out] frag_index 当前片序号（0 起始）
         * @param[out] frag_total 总片数
         * @return 头部合法返回 true，否则 false
         * @details 校验魔数、片长一致性，以及片序号与总片数的逻辑关系。
         */
        static bool ParseHeader(const std::vector<uint8_t>& chunk,
            uint16_t& seq, uint8_t& frag_index, uint8_t& frag_total);

        /**
         * @brief 重组器（用于接收侧收片拼包）
         * @details 维护一个按报序号分组的 pending 表，每个条目包含已收到的分片。
         *          收到分片后插入对应序号，若收齐则拼接完整数据并返回。
         *          超时（30s）未收齐的报文会被自动清理。
         */
        class Reassembler {
        public:
            /**
             * @brief 投递一个分片，尝试重组完整报文
             * @param chunk 分片数据（含完整头部）
             * @param[out] out 如果返回 true，则输出完整报文数据
             * @return true 表示已收齐一个完整报文（out 有效），false 表示仍在等待更多分片或分片非法
             * @note 已完成的报文会从 pending 表中移除，调用者应获取 out 后继续处理。
             */
            bool Push(const std::vector<uint8_t>& chunk, std::vector<uint8_t>& out);

            /// 清空所有 pending 状态
            void Clear();

        private:
            /**
             * @brief 单个报文的 pending 状态
             */
            struct Pending {
                uint8_t  frag_total = 0;                      ///< 总片数
                uint8_t  received = 0;                        ///< 已收到的片数
                size_t   bytes = 0;                           ///< 已缓存负载字节数(D6: 总量上限判定)
                std::map<uint8_t, std::vector<uint8_t>> parts; ///< 片序号 → 数据（按序存储）
                uint64_t last_seen_ms = 0;                    ///< 最后一次收到该报文的时间戳（毫秒）
            };

            std::map<uint16_t, Pending> pending_;  ///< 报序号 → pending 状态
            /**
             * @brief 获取当前时间戳（毫秒）
             * @return 当前单调时钟的毫秒值
             */
            uint64_t now_ms() const;
        };
    };

} // namespace cleat