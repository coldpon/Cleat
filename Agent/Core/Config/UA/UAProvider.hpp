/**
 * @file UAProvider.hpp
 * @brief 浏览器 User-Agent 统一提供器(全通道/死投递共用的伪装资产)。
 * @details
 * 背景: UA 池此前散落 6 处私有副本(HTTPS Win/Linux 平台层、WS 平台层、
 * WS 握手编解码、TCP 伪装壳、死投递硬编码单条), 各池内容漂移, 且远程下发
 * (http.user_agent)只覆盖 HTTPS 一处。收敛为单一组件后:
 *   * **单一池**(按平台条件编译: Win 构建 = Windows 系 UA, Linux 构建 = Linux 系
 *     UA —— 平台伪装一致性是刻意设计, 两池不合并; 内容 = 原各副本并集去重);
 *   * **配置即真相**: 每次取用直读 AgentConfig::GetHttpUserAgent() —— 远程下发
 *     (http.user_agent)/嵌入式配置/编译期默认自动生效, **无需任何推送接线**
 *     (http_user_agent 字段公开直写的既有模式也不会漏); 非空恒用之, 空 = 池内
 *     随机 —— 与原 GetHttpUserAgent 消费方语义完全兼容;
 *   * 位置: Core/Config/ —— 它是 http_user_agent 配置的池化视图(伴生件),
 *     消费方(Network 各通道 / Exchange 死投递)引本头即可, 不必各自持有池副本。
 * 线程安全: GetHttpUserAgent 内部读锁; 池 rng 由本类锁串行化。
 */
#pragma once

#include <string>

namespace cleat {

    class UAProvider {
    public:
        UAProvider() = delete;
        UAProvider(const UAProvider&) = delete;
        UAProvider& operator=(const UAProvider&) = delete;

        /// 取一个 UA: 配置(http.user_agent)非空 → 配置值; 否则平台池内随机。线程安全。
        static std::string Pick();
    };

} // namespace cleat
