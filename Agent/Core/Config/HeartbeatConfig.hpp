/**
 * @file HeartbeatConfig.hpp
 * @brief 心跳调度参数(纯数据, 零依赖) —— 从 HeartbeatPolicy.hpp 抽出(P2.6 解环)
 * @details
 * **为什么单独成头**: `AgentConfig.hpp` 只需要 `HeartbeatConfig` 这个纯数据结构, 却曾
 * include 整个 `HeartbeatPolicy.hpp`(含策略类 + `<random>` + `<mutex>`), 造成
 * `Core/Config → Core/Comms` 的**头文件级循环边**(Config 与 Comms 互指)。
 * 把纯数据抽到本头后, `AgentConfig.hpp` 只依赖本头; `HeartbeatPolicy.hpp` 反向包含本头。
 *
 * **落位说明**: 归 **Core/Config/** —— 本结构是 `AgentConfig::heartbeat_`
 * 的字段类型, 属配置语义; 抽出后两处 include 均为同域或既定的 `Comms → Config` 单向,
 * 不构成环。核心领域概念留在 Core 内, 不下沉 Common。
 *
 * 参数语义见各字段注释; 服务器可通过配置通道(SetConfig)动态下发更新。
 */
#pragma once

#include <cstdint>

namespace cleat {

    /**
     * @brief 心跳调度参数结构
     * @details
     * 服务器可通过配置通道（SetConfig）动态推送更新，默认值在结构体初始化时设定。
     * 所有时间单位均为毫秒，抖动和丢拍概率以千分比（‰）表示。
     */
    struct HeartbeatConfig {
        uint32_t base_interval_ms = 60000;     ///< 空闲状态基础心跳间隔（毫秒）
        uint32_t jitter_ratio_permille = 200;  ///< 均匀抖动幅度（±‰，相对于基础间隔）
        uint32_t active_interval_ms = 15000;   ///< 活跃状态下收紧的心跳间隔（毫秒）
        uint32_t activity_window_ms = 30000;   ///< 活动判定窗口：此时间内有命令响应视为活跃

        /**
         * @brief 抖动分布类型
         * @details 0 = 均匀分布（历史默认行为），1 = 正态分布（均值 = base + bias，标准差 = jitter_std）
         */
        uint32_t jitter_distribution = 1;

        /**
         * @brief 正态分布标准差（‰，相对于基础间隔）
         * @details 若为 0，则回退使用 jitter_ratio_permille 作为标准差
         */
        uint32_t jitter_std_permille = 250;

        /**
         * @brief 正态分布均值偏移（‰，相对于基础间隔，可为负值）
         * @details 例如 +150 表示均值偏向长尾，产生更长的心跳间隔
         */
        int32_t jitter_mean_bias_permille = 100;

        /**
         * @brief 随机心跳丢拍概率（‰，0-1000）
         * @details 命中时本次心跳不发，使节拍不规则化
         */
        uint32_t skip_beat_permille = 30;
    };

} // namespace cleat
