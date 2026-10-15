/**
 * @file HeartbeatPolicy.hpp
 * @brief 心跳调度策略定义
 * @details
 * 提供自适应心跳间隔调度，支持均匀分布和正态分布两种抖动模式，
 * 用于抵抗流量周期建模分析。
 * 所有配置参数可通过远程配置通道（SetConfig）动态下发。
 */

#pragma once
#include <cstdint>
#include <mutex>
#include <random>

#include "Core/Config/HeartbeatConfig.hpp"   // HeartbeatConfig(P2.6 解环: 纯数据单独成头)

namespace cleat {

    /**
     * @brief 带抖动的心跳策略类
     * @details
     * 计算下一次心跳间隔，支持基于活动状态的动态调整。
     * 所有配置更新和读取均为线程安全（使用互斥锁）。
     * 纯标准库实现，无平台依赖，适用于 Core 层。
     */
    class HeartbeatPolicy {
    public:
        /**
         * @brief 更新心跳配置（线程安全）
         * @param cfg 新的心跳配置
         * @details
         * M2 修复：此函数在 SetConfig 链（worker 线程）中调用，
         * 与 GetConfig/NextIntervalMs（SessionLoop 主循环）并发访问，
         * 通过互斥锁保护 cfg_。
         */
        void UpdateConfig(const HeartbeatConfig& cfg) {
            std::lock_guard<std::mutex> lock(mutex_);
            cfg_ = cfg;
        }

        /**
         * @brief 获取当前心跳配置副本（线程安全）
         * @return 当前配置的副本
         */
        HeartbeatConfig GetConfig() const {
            std::lock_guard<std::mutex> lock(mutex_);
            return cfg_;   // 拷贝返回，调用方不持锁
        }

        /**
         * @brief 计算下一次心跳间隔
         * @param active 当前是否处于活跃状态（有最近命令活动）
         * @return 下一次心跳的间隔（毫秒）
         * @details
         * 根据配置的分布类型（均匀或正态）生成随机间隔，
         * 下限固定为 1000ms 防止忙等。
         */
        uint32_t NextIntervalMs(bool active);

        /**
         * @brief 判断本次心跳是否应被跳过（随机丢拍）
         * @return true 表示跳过本次心跳，false 表示正常发送
         * @details 基于配置的 skip_beat_permille 概率判断。
         */
        bool ShouldSkipBeat();

        /**
         * @brief 注入随机种子
         * @param seed 0 = 恢复随机种子
         * @note 仅影响抖动/丢拍的抽样取值, 不改变任何策略逻辑
         */
        void Seed(uint32_t seed) {
            rng_.seed(seed == 0 ? std::random_device{}() : seed);
        }

    private:
        HeartbeatConfig cfg_;                ///< 当前心跳配置
        std::mt19937 rng_{ std::random_device{}() }; ///< 随机数生成器
        mutable std::mutex mutex_;           ///< 保护 cfg_ 的互斥锁
    };

} // namespace cleat