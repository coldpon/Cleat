/**
 * @file OutboundQueue.cpp
 * @brief 出站队列实现
 */

#include "OutboundQueue.hpp"

#include "Common/Logger/Logger.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace cleat {

    namespace {
        /// B2-2：关键帧识别标签 —— FirstConnect 注册帧（JSON 中包含此命令字符串）
        const std::string FirstConnectTag = "FirstConnect";
    }

    /**
     * @brief 构造函数
     * @param max_bytes 队列最大容量（字节）
     */
    OutboundQueue::OutboundQueue(size_t max_bytes)
        : max_bytes_(max_bytes) {
    }

    /**
     * @brief 入队一帧
     * @param frame 数据帧
     * @return 始终返回 true
     * @details
     * 加锁保护，入队后若超限则丢弃最旧的帧。
     * 丢弃策略优先丢弃非关键帧（X25519 握手 "KE" 和 FirstConnect 注册帧），
     * 最多跳过 8 个关键帧，若全是关键帧则兜底丢弃最旧的。
     */
    bool OutboundQueue::Push(std::vector<uint8_t> frame) {
        std::lock_guard<std::mutex> lock(mutex_);
        bytes_ += frame.size();
        frames_.push_back(std::move(frame));

        // 超限：丢弃最旧（保留最新），防止失控积压；单帧超限时兜底保留该帧。
        //关键帧（X25519 握手 "KE" / FirstConnect 注册）受保护 ——
        // 断线积压场景丢最旧可能恰好丢刚回插的关键帧（会话注册/握手），导致重连后
        // 永远无法完成协商；丢弃时优先找最旧的【非关键帧】，最多跳过 8 个关键帧。
        size_t dropped_count = 0;
        while (bytes_ > max_bytes_ && frames_.size() > 1) {
            auto it = frames_.begin();
            int scanned = 0;
            while (it != frames_.end() && scanned < 8) {
                const auto& f = *it;
                const bool is_key =
                    (f.size() >= 2 && f[0] == 'K' && f[1] == 'E') ||   // X25519 握手帧
                    (f.size() >= 24 && std::search(f.begin(), f.end(),
                        FirstConnectTag.begin(), FirstConnectTag.end()) != f.end());
                if (!is_key) break;
                ++it;
                ++scanned;
            }
            if (it == frames_.end()) {
                it = frames_.begin();   // 全关键帧/跳满：兜底丢最旧
            }
            bytes_ -= it->size();
            frames_.erase(it);
            ++dropped_count;
        }
        if (dropped_count > 0) {
            LOG_WARN("OutboundQueue: 超限丢弃 " << dropped_count << " 帧"
                << " (当前 " << bytes_ << "B / 上限 " << max_bytes_ << "B)");
        }
        return true;
    }

    /**
     * @brief 出队一帧（队首）
     * @param[out] frame 输出帧数据
     * @return 队列非空返回 true，否则 false
     */
    bool OutboundQueue::Pop(std::vector<uint8_t>& frame) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (frames_.empty()) {
            return false;
        }
        frame = std::move(frames_.front());
        bytes_ -= frame.size();
        frames_.pop_front();
        return true;
    }

    /**
     * @brief 将一帧插到队首（用于重发）
     * @param frame 待插入的帧
     */
    void OutboundQueue::PushFront(std::vector<uint8_t> frame) {
        std::lock_guard<std::mutex> lock(mutex_);
        bytes_ += frame.size();
        frames_.push_front(std::move(frame));
    }

    /**
     * @brief 检查队列是否为空
     */
    bool OutboundQueue::Empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return frames_.empty();
    }

    /**
     * @brief 获取当前队列占用的总字节数
     */
    size_t OutboundQueue::Bytes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return bytes_;
    }

    /**
     * @brief 清空队列
     */
    void OutboundQueue::Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        frames_.clear();
        bytes_ = 0;
    }

} // namespace cleat