/**
 * @file ChannelFieldRules.hpp
 * @brief 通道字段规则表(P2 彻底合流): "一个通道要哪些字段才算配好"的**唯一**数据源。
 * @details
 * **为什么要有这个头**:
 *   收敛前"该通道是否被正确配置"这条判断被抄了两份互不校验的真相:
 *     (1) `ChannelSpec::Valid()`   —— Core/Config/AgentConfig.cpp 的 per-type if 链
 *     (2) `ChannelDescriptor::valid` —— Core/Comms/Channel/ChannelRegistry.cpp 的 lambda
 *   两份语义本应一致却靠人眼维持; 加通道漏改其一 → "配置校验说无效、热切换却能过"
 *   (或反之), 是最难定位的一类静默失效(实测两处逐条比对**已验证完全同义**)。
 *
 * **本头把规则提出来**: 纯函数 + 字符串键, **零依赖**(不含 AgentConfig, 不引
 * ChannelRegistry) —— 于是配置层(校验期)与通信层(装配期)都能 include, 而不制造
 * Config <-> Channel 的循环依赖(Channel/Chain.cpp 已反向 include AgentConfig.hpp)。
 *
 * **加通道时**: 若该通道的"配好判据"不是"总是合法", 在 `ChannelFieldsSatisfied`
 * 加一个 if —— 但**只此一处**; 描述符的 valid 与 ChannelSpec::Valid 都转调它。
 *
 * **落位说明**: 本头原置于 Core/Comms/Channel/, 但 Core/Config 的
 * 校验期也要用它 → 若留在 Network 里会被依赖体检读成 Config→Network 反向依赖(叠加
 * Chain.cpp 反向依赖 AgentConfig.hpp, 即成头文件环)。故迁出 Network 侧。
 *
 * 归属地选定 **Core/Config/**(而非 Common/): 本头表达的是"配置是否配好"这条**配置语义**,
 * 与 `AgentConfig` 同域; 且它零依赖, 搬入后 `AgentConfig.hpp` 的 include 变为同域引用,
 * 跨域边只剩既定的 `Network → Config` 单向(`PollClock.hpp`/`Chain.cpp` 同向),
 * **不构成环**(`scripts/dep_audit.py` 复验真环 0)。核心领域概念留在 Core 内, 不下沉 Common。
 */
#pragma once

#include <cstdint>
#include <string>

namespace cleat {

    /**
     * @brief 通道配置输入的字段子集(与 ChannelConfigInput / ChannelSpec 的公共字段一致)
     * @details 只列"判据会用到"的字段, 避免本头依赖 ChannelRegistry.hpp(保持零依赖)。
     *          调用方把各自的结构体字段拷进来即可。
     */
    struct ChannelFields {
        std::string host;
        std::string url;           ///< https/wss: 全 URL 或路径
        std::string dns_server;    ///< dns
        std::string dns_domain;    ///< dns
        std::string icmp_target;   ///< icmp
    };

    /**
     * @brief 该通道类型"配好"的判据(单一真相)
     * @details 语义与收敛前的 ChannelSpec::Valid / 各描述符 valid **逐条等价**:
     *            tcp   : 总是合法(host 空 = 沿用启动 -host/-port)
     *            https : url 或 host 至少一个非空
     *            dns   : dns_server 与 dns_domain 都非空
     *            icmp  : icmp_target 非空
     *            pipe  : 总是合法(口令可选; Pivot 自建)
     *            其它  : 非法(未知类型)
     * @note quic 判据比 wss 更紧(要求 quic:// 前缀而非任意 url): 通用 url 槽在生产
     *       启动路径装的是 https_url, 宽松版会让"配了 https 的产物"把它误判为已配置。
     *       收紧后 valid 与 from_config 同判据 —— 通过判据即可物化, 不产生"过了这道
     *       闸、卡在下道闸"的观感差。
     * @return true = 足以物化该通道
     */
    inline bool ChannelFieldsSatisfied(const std::string& type, const ChannelFields& f) {
        if (type == "tcp" || type == "pipe") return true;
        if (type == "https" || type == "wss") return !f.url.empty() || !f.host.empty();
        if (type == "quic") return (!f.url.empty() && f.url.rfind("quic://", 0) == 0)
            || !f.host.empty();
        if (type == "dns") return !f.dns_server.empty() && !f.dns_domain.empty();
        if (type == "icmp") return !f.icmp_target.empty();
        return false;
    }

    /**
     * @brief 该通道是否依赖平台工厂(SelfCheck 的 (c) 类检查用)
     * @details 取代 ChannelRegistry::SelfCheck 里 `type == "tcp" || ...` 的手写枚举。
     * @return 依赖工厂的类型名单见 ChannelDepKindFor
     */
    inline bool ChannelNeedsDep(const std::string& type) {
        return type == "tcp" || type == "https" || type == "dns"
            || type == "icmp" || type == "wss" || type == "pipe" || type == "quic";
    }

    /**
     * @brief 该通道需要的平台工厂种类(供 SelfCheck 判断"当前平台是否提供")
     * @return 0=不需要, 1=net_ops(TCP/DNS/WSS), 2=make_http, 3=make_icmp, 4=make_pipe,
     *         5=make_quic(msquic; Windows 提供, 其他平台暂缺)
     */
    enum class ChannelDepKind : int {
        None = 0,
        NetOps = 1,
        Http = 2,
        Icmp = 3,
        Pipe = 4,
        Quic = 5,
    };

    inline ChannelDepKind ChannelDepKindFor(const std::string& type) {
        if (type == "tcp" || type == "dns" || type == "wss") return ChannelDepKind::NetOps;
        if (type == "quic") return ChannelDepKind::Quic;
        if (type == "https") return ChannelDepKind::Http;
        if (type == "icmp") return ChannelDepKind::Icmp;
        if (type == "pipe") return ChannelDepKind::Pipe;
        return ChannelDepKind::None;
    }

} // namespace cleat
