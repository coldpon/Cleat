/**
 * @file ChannelRateLimiter.hpp
 * @brief 低带宽通道报文级限流器
 * @details
 * 位置：业务出口闸门 —— AgentContext::SendData 统一收口处，所有出站帧
 *       （命令响应/主动推送）必经；仅当活动通道为低带宽（DNS/ICMP）时启用。
 *
 * 三位一体（对"一次性报文"的统一策略，与 ServiceManager 启停级检查、
 *       CheckinThrottle 通道节奏级节流互不重叠）：
 *   1. 命令类型：deny 黑名单命中 → 直接拒绝（压根出不去）
 *   2. 载荷大小：单帧载荷超过该命令 max_bytes → 拒绝
 *   3. 频率：滑动窗口（60s）内该命令发送次数达到 rate_per_min → 拒绝
 *
 * 超限行为：返回决策码，由调用方（SendData）回错误帧给服务器 —— 不静默丢弃，
 *       保证操作者有感知（前端可显示"被通道限流"）。
 * 配置：从 AgentConfig::GetChannelLimit() 读取，SetConfig 下发即时生效。
 * 线程安全：配置读取加锁 + 频率桶内部互斥，多线程可并发 Check。
 */

#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

namespace cleat {

    /**
     * @brief 低带宽通道限流器
     */
    class ChannelRateLimiter {
    public:
        /// 限流决策结果
        enum class Decision {
            Allow,          ///< 放行
            Deny,           ///< 命令被黑名单拒绝
            TooLarge,       ///< 载荷超过该命令大小上限
            RateLimited,    ///< 超过该命令频率配额（滑动窗口）
        };

        ChannelRateLimiter() = default;
        ~ChannelRateLimiter() = default;

        ChannelRateLimiter(const ChannelRateLimiter&) = delete;
        ChannelRateLimiter& operator=(const ChannelRateLimiter&) = delete;
        ChannelRateLimiter(ChannelRateLimiter&&) = delete;
        ChannelRateLimiter& operator=(ChannelRateLimiter&&) = delete;

        /**
         * @brief 发送前检查（低带宽通道专用）
         * @param cmd           命令名（响应/推送帧的 cmd 字段；空 = 未知命令走 default 规则）
         * @param payload_bytes 整帧载荷大小（字节）
         * @return 限流决策
         * @note 内部加锁、不抛异常；0 上限 / 0 配额表示对应维度不限制
         */
        Decision Check(const std::string& cmd, size_t payload_bytes);

        /**
         * @brief 清空频率统计（通道切换 / 重置时可调用，可选）
         */
        void Reset();

        /// 决策 → 错误码（错误帧的 code 字段，稳定字符串供 server 匹配）
        static const char* CodeOf(Decision d);

        /// 决策 → 人类可读原因（错误帧 message / 日志）
        static const char* ReasonOf(Decision d);

    private:
        /// 当前单调时钟毫秒
        static int64_t NowMs();

        std::mutex mutex_;                                              ///< 频率桶保护
        std::unordered_map<std::string, std::deque<int64_t>> buckets_;  ///< 每命令滑动窗口时间戳
    };

} // namespace cleat