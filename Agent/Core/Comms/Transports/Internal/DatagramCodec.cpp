/**
 * @file DatagramCodec.cpp
 * @brief 数据报编解码器实现
 * @details 实现分片、头部解析及重组器逻辑。
 */

#include "DatagramCodec.hpp"

#include <algorithm>
#include <chrono>
#include <atomic>

namespace cleat {

    /**
     * @brief 将数据分片为多个数据报分片
     * @param data 待分片的原始数据
     * @param max_frag_payload 每片最大负载长度
     * @return 分片列表
     * @details
     * 使用原子递增的报序号（从 0 开始，16 位回绕）。
     * 每个分片包含 7 字节头部，然后按顺序填充负载数据。
     */
    std::vector<std::vector<uint8_t>> DatagramCodec::Fragment(
        const std::vector<uint8_t>& data, size_t max_frag_payload) {
        std::vector<std::vector<uint8_t>> out;
        if (max_frag_payload == 0) {
            max_frag_payload = 1;
        }
        // 报序号：递增，回绕安全（重排按 16bit 短窗口）
        static std::atomic<uint16_t> next_seq{ 0 };
        const uint16_t seq = next_seq.fetch_add(1, std::memory_order_relaxed);

        const size_t total = (data.size() + max_frag_payload - 1) / max_frag_payload;
        // static_cast<uint8_t> 截断 → 接收侧按截断后的 total 永远收不齐,
        // 大帧静默丢失且发送侧仍报成功。超过协议上限时显式失败(返回空),
        // 由上层感知并走降级, 不再假装成功。
        if (total > 255) {
            return out;
        }
        for (size_t i = 0; i < total; ++i) {
            const size_t off = i * max_frag_payload;
            const size_t n = std::min(max_frag_payload, data.size() - off);
            std::vector<uint8_t> chunk;
            chunk.reserve(kHeaderSize + n);
            // 填充头部
            chunk.push_back(kMagic);
            chunk.push_back(static_cast<uint8_t>(seq & 0xFF));
            chunk.push_back(static_cast<uint8_t>((seq >> 8) & 0xFF));
            chunk.push_back(static_cast<uint8_t>(i));
            chunk.push_back(static_cast<uint8_t>(total));
            chunk.push_back(static_cast<uint8_t>(n & 0xFF));
            chunk.push_back(static_cast<uint8_t>((n >> 8) & 0xFF));
            // 复制负载
            chunk.insert(chunk.end(), data.begin() + off, data.begin() + off + n);
            out.push_back(std::move(chunk));
        }
        return out;
    }

    /**
     * @brief 解析分片头部
     * @param chunk 分片数据
     * @param[out] seq 报序号
     * @param[out] frag_index 片序号
     * @param[out] frag_total 总片数
     * @return 头部合法返回 true，否则 false
     */
    bool DatagramCodec::ParseHeader(const std::vector<uint8_t>& chunk,
        uint16_t& seq, uint8_t& frag_index, uint8_t& frag_total) {
        if (chunk.size() < kHeaderSize || chunk[0] != kMagic) {
            return false;
        }
        seq = static_cast<uint16_t>(chunk[1]) | (static_cast<uint16_t>(chunk[2]) << 8);
        frag_index = chunk[3];
        frag_total = chunk[4];
        const size_t frag_len = static_cast<size_t>(chunk[5])
            | (static_cast<size_t>(chunk[6]) << 8);
        // 校验片序号与总片数逻辑一致性，以及片长与负载实际长度匹配
        if (frag_total == 0 || frag_index >= frag_total ||
            frag_len != chunk.size() - kHeaderSize) {
            return false;
        }
        return true;
    }

    /**
     * @brief 获取当前时间戳（毫秒）
     * @return 从 steady_clock  epoch 开始的毫秒数
     */
    uint64_t DatagramCodec::Reassembler::now_ms() const {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    /**
     * @brief 投递一个分片，尝试重组完整报文
     * @param chunk 分片数据
     * @param[out] out 如果返回 true，则输出完整报文
     * @return true 表示已收齐一个完整报文，false 表示等待更多分片或分片非法
     * @details
     * 先清理超时（30s）的 pending 条目，然后按报序号查找或新建 pending 状态。
     * 插入分片数据，若该报文收齐所有片，则按片序号顺序拼接并返回。
     */
    bool DatagramCodec::Reassembler::Push(const std::vector<uint8_t>& chunk,
        std::vector<uint8_t>& out) {
        uint16_t seq = 0;
        uint8_t index = 0, total = 0;
        if (!ParseHeader(chunk, seq, index, total)) {
            return false;   // 非法分片：丢弃（不视为完成）
        }

        const uint64_t now = now_ms();

        // 清理过期报文（单遍扫描，30s 未更新的丢弃），同时累计在途字节
        size_t pending_bytes = 0;
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (now - it->second.last_seen_ms > kReassemblyTimeoutMs) {
                it = pending_.erase(it);
            }
            else {
                pending_bytes += it->second.bytes;
                ++it;
            }
        }

        // 伪造任意 seq 灌爆重组表, 且每片到达都做一次全表扫描(CPU 放大)。
        // 清理过期后仍达上限则拒绝新 seq(丢弃本片), 有界后扫描成本可控。
        //  精修: ① 在途报文(seq 已存在)不受条目上限影响 —— 否则表满时
        // 连"续片"都被拒, 在途报文永远收不齐; ② 条目数不封字节(单片负载可达
        // 64KB, 128 条 ≈ 16MB×128), 另加在途总字节上限。
        const size_t payload_len = chunk.size() - kHeaderSize;
        if (pending_.find(seq) == pending_.end()
            && pending_.size() >= kMaxPendingEntries) {
            return false;
        }
        if (pending_bytes + payload_len > kMaxPendingBytes) {
            return false;
        }

        auto it = pending_.find(seq);
        if (it == pending_.end()) {
            it = pending_.emplace(seq, Pending{}).first;
        }
        auto& p = it->second;
        if (p.frag_total != 0 && p.frag_total != total) {
            // 同报序号但总片数不一致：协议错乱，丢弃旧报文重新开始。
            // 后重新 pending_[seq], 但上面已绑定的引用 p 指向被 erase 释放的节点,
            // 随后的 p.last_seen_ms = now 属 use-after-free(写已释放结点)。
            // 明文通道(DNS/ICMP)对端可构造同 seq 异 total 触发该路径。
            p = Pending{};
        }
        p.frag_total = total;
        p.last_seen_ms = now;

        // 插入分片，若该片未重复则增加已收计数与字节数(重复片不计, 防重复片放大)
        if (p.parts.emplace(index, std::vector<uint8_t>(
            chunk.begin() + kHeaderSize, chunk.end())).second) {
            ++p.received;
            p.bytes += payload_len;
        }

        // 检查是否收齐
        if (p.received == p.frag_total) {
            // 按片序号拼接
            out.clear();
            for (auto& kv : p.parts) {
                out.insert(out.end(), kv.second.begin(), kv.second.end());
            }
            pending_.erase(seq);
            return true;
        }
        return false;
    }

    /**
     * @brief 清空所有 pending 状态
     */
    void DatagramCodec::Reassembler::Clear() {
        pending_.clear();
    }

} // namespace cleat