/**
 * @file UAProvider.cpp
 * @brief UAProvider 实现(池按平台条件编译, 见头文件注)。
 */
#include "UAProvider.hpp"

#include "Core/Config/AgentConfig.hpp"

#include <mutex>
#include <random>

namespace cleat {

    namespace {
        // 平台池(条件编译): Windows 构建放 Windows 系 UA, Linux 构建放 Linux 系 UA
        // —— 平台伪装一致性是刻意设计(与原 PlatformLinux_Http 的决策一致), 两池
        // 不合并。内容 = 原各通道私有副本的并集(去重)。
#if defined(_WIN32)
        const char* const kPool[] = {
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36",
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/125.0.0.0 Safari/537.36",
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36 Edg/124.0.0.0",
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/125.0.0.0 Safari/537.36 Edg/125.0.0.0",
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:126.0) Gecko/20100101 Firefox/126.0",
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:127.0) Gecko/20100101 Firefox/127.0",
        };
#else
        const char* const kPool[] = {
            "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36",
            "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/125.0.0.0 Safari/537.36",
            "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36 Edg/124.0.0.0",
            "Mozilla/5.0 (X11; Ubuntu; Linux x86_64; rv:126.0) Gecko/20100101 Firefox/126.0",
            "Mozilla/5.0 (X11; Linux x86_64; rv:125.0) Gecko/20100101 Firefox/125.0",
            "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chromium/126.0.0.0 Chrome/126.0.0.0 Safari/537.36",
        };
#endif

        std::mutex& PoolMutex() {
            static std::mutex m;
            return m;
        }
    } // namespace

    std::string UAProvider::Pick() {
        // 配置即真相: http.user_agent 非空恒用(远程下发/嵌入式配置/编译期默认
        // 自动生效, 无需任何推送接线 —— 字段公开直写的既有模式也不会漏)。
        const std::string configured = AgentConfig::Instance().GetHttpUserAgent();
        if (!configured.empty()) {
            return configured;
        }
        std::lock_guard<std::mutex> lock(PoolMutex());
        // rng 访问被本锁串行化(mt19937 并发调用是数据竞争, 锁内即安全)
        static std::mt19937 rng{ std::random_device{}() };
        return kPool[rng() % (sizeof(kPool) / sizeof(kPool[0]))];
    }

} // namespace cleat
