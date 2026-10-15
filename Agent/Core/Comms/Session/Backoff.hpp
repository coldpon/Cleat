/**
 * @file Backoff.hpp
 * @brief 退避策略(指数增长 + 抖动)
 * @details
 * 合一前: `ConnectionManager` 里**两份**几乎相同的退避逻辑 —— 首连一份
 * (`reconnect.first_connect_base_ms/max_ms`)、重连一份(`base_ms/max_ms`),
 * 各自维护 `retry_delay`、各自 `min(x*2, max)`、各自调用 `JitteredDelay`。
 * 两份实现的参数与语义都不同步(改一处漏一处), 且无法单独测试。
 *
 * 合一后: 一份策略对象, 由配置构造; 调用方只问"这次等多久"。
 *
 * 抖动的作用是**防惊群**(服务器重启后大量 agent 同时重连会冲垮服务端) ——
 * 服务端侧亦有防抖(5s 内 LastSeen 拒绝新连接并回 DENY_BUSY), 两端协同见
 *
 * 确定性: 可注入随机种子以便夹具断言**精确序列**(否则只能断言边界与单调性)。
 */
#pragma once

#include <cstdint>
#include <random>

namespace cleat {

    class Backoff {
    public:
        /**
         * @param base_ms          首次退避基准
         * @param max_ms           退避上限(指数增长到此为止)
         * @param jitter_permille  抖动千分比(±): 最终等待 = delay ± delay*jitter/1000
         * @param seed             0 = 用随机种子(生产); 非 0 = 固定种子(夹具确定性)
         */
        Backoff(uint32_t base_ms, uint32_t max_ms, uint32_t jitter_permille,
            uint32_t seed = 0);

        /**
         * @brief 取本次应等待的时长并推进内部基准
         * @return 带抖动的等待毫秒数(下限 100ms, 防忙等)
         * @details 语义与合一前的 `JitteredDelay` + `min(x*2, max)` **逐条一致**:
         *          先按当前基准算抖动值返回, 再把基准翻倍(不超上限)。
         */
        uint32_t NextDelayMs();

        /// 当前基准(未翻倍前); 供观测与断言
        uint32_t CurrentBaseMs() const { return current_; }

        /// 复位到初始基准(连接成功后调用, 让下一次失败从最短退避重新开始)
        void Reset();

    private:
        uint32_t base_;
        uint32_t max_;
        uint32_t jitter_permille_;
        uint32_t current_;
        std::mt19937 rng_;
    };

} // namespace cleat
