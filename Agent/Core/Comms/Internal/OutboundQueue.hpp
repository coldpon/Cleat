/**
 * @file OutboundQueue.hpp
 * @brief 有界出站队列（Phase 0 S4）
 * @details
 * 解耦“模块 Send”与“传输落线” —— 所有模块把帧投递到队列，由 CommsManager 统一刷出。
 * TCP（流式）立即刷（行为与直发等价）；HTTPS/DNS/ICMP（请求-响应）每 poll 周期刷（Phase 1+ 使用）。
 * 有界：超过 max_bytes 时丢弃最旧的帧并告警，防止失控模块（如高帧率服务）无限积压。
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace cleat {

    /**
     * @brief 出站队列类
     * @details
     * 管理待发送的数据帧，提供有界缓存和线程安全的入队/出队操作。
     * 内部使用 std::deque 存储，支持从队首和队尾操作。
     */
    class OutboundQueue {
    public:
        /**
         * @brief 构造函数
         * @param max_bytes 队列最大容量（字节），默认 16 MiB
         */
        explicit OutboundQueue(size_t max_bytes = 16 * 1024 * 1024);

        /**
         * @brief 入队一帧
         * @param frame 待发送的数据帧（移动语义）
         * @return 始终返回 true（有界兜底，总是接收）
         * @details
         * 若队列大小超过 max_bytes，则会丢弃最旧的帧（非关键帧优先）并记录警告。
         * 关键帧（X25519 握手 "KE" 和 FirstConnect 注册帧）受到保护，尽量保留。
         */
        bool Push(std::vector<uint8_t> frame);

        /**
         * @brief 出队一帧（队首）
         * @param[out] frame 输出的数据帧
         * @return 队列非空时返回 true，否则 false
         */
        bool Pop(std::vector<uint8_t>& frame);

        /**
         * @brief 将一帧插到队首（用于重发）
         * @param frame 待插入的数据帧
         * @details 通常用于发送失败的帧需要保留并重发时（如  场景）。
         */
        void PushFront(std::vector<uint8_t> frame);

        /// 检查队列是否为空
        bool Empty() const;

        /// 获取当前队列占用的总字节数
        size_t Bytes() const;

        /// 容量上限(字节) ——  出口据此**预检**容量, 以兑现
        /// `SendOptions::allow_drop`(Push 本身恒接收, 超限时静默丢最旧)
        size_t MaxBytes() const { return max_bytes_; }

        /// 清空队列
        void Clear();

    private:
        std::deque<std::vector<uint8_t>> frames_;  ///< 帧存储容器
        size_t bytes_ = 0;                         ///< 当前队列总字节数
        const size_t max_bytes_;                   ///< 队列容量上限
        mutable std::mutex mutex_;                 ///< 保护所有操作的互斥锁
    };

} // namespace cleat