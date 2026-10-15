/**
 * @file AgentContextInternals.h
 * @brief 身份重构⑤: AgentContext 拆三 TU 共享的 TU 内辅助函数。
 * @details
 * 原 AgentContext.cpp 匿名 namespace 的 helper 被三个实现文件共用
 * (AgentContext.cpp / AgentContextAssemble.cpp / AgentContextRuntime.cpp),
 * 提升为本头文件(inline, 多 TU 安全)。**仅限 Composition/AgentContext*.cpp 包含**
 * (Composition 拆分 : 本头随装配根从 Entry/ 迁入, 内容仅路径类更新)。
 *
 * **P1 收敛**: 原先散落在这里的
 *   - ChannelOrderForMode       → 已收进 ChannelRegistry::BootstrapOrder
 *   - BuildBootstrapChannelConfig → 已成为 ChannelDescriptor::from_config
 *   - BuildChannelConfigFromSpec  → 同上
 * 装配层不再"逐类型 if-else 翻译", 只做**来源归一**: 把 AgentConfig(启动扁平字段)
 * 或 ChannelSpec(热切换下发) 都归一成 ChannelConfigInput, 交给注册表。
 * 这样"一个通道如何从配置变成 TransportConfig"在整个工程只有一份实现。
 *
 * **候选链物化收口**: MaterializeChannels 成为候选链物化的**唯一**
 * 实现 —— SetupCommsLayer 与 ReloadChannels 此前各持一份逐段同构的手写副本
 * (deps 填充各自维护, 曾因两路径不对称导致 QUIC 热切换丢失的 P4 缺陷), 此后
 * 两路径只做"输入来源归一", 全部可观察差异收进 MaterializePolicy; ChannelDeps
 * 亦收成唯一构造点 BuildChannelDeps。
 */

#pragma once

#include "Core/Config/AgentConfig.hpp"
#include "Core/Comms/Channel/ChannelRegistry.hpp"
#include "Core/Config/ChannelFieldRules.hpp"
#include "Core/Comms/Transports/ITransport.hpp"
#include "Core/Comms/Channel/Chain.hpp"
#include "Common/Logger/Logger.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace cleat {
namespace agentctx_int {

    /// url/upload_url 组装: 带 scheme 直接用; 仅路径时用 host[:port] 打底
    inline std::string BuildUrl(const std::string& raw, const std::string& host,
        uint16_t port) {
        if (raw.rfind("http://", 0) == 0 || raw.rfind("https://", 0) == 0) {
            return raw;
        }
        if (host.empty()) return "";
        std::string out = "https://" + host;
        if (port != 0) out += ":" + std::to_string(port);
        if (!raw.empty()) out += (raw[0] == '/') ? raw : ("/" + raw);
        return out;
    }

    /**
     * @brief 启动扁平字段 → 通道配置输入(P1 收敛: 取代 BuildBootstrapChannelConfig)
     * @details 只做**字段搬运**, 不做类型判定 —— 是否"该通道被配置了"由描述符的
     *          from_config/valid 决定。timeout 由调用方统一注入。
     *
     * **P2 彻底合流**: 历史遗留把 https 与 wss 的 url 分成 `https_url` /
     * `wss_url` 两个槽。此前本函数按 `type == "wss"` 选槽 —— 特判只是从装配层挪到了
     * 这里, 并未消除。现在**两个槽无条件都填**, 选槽的责任归还给"通道专属逻辑"的
     * 归属地(wss 描述符的 from_config), 本函数对 type 完全无感。
     */
    inline ChannelConfigInput FromAgentConfig(const AgentConfig& cfg, const std::string&) {
        ChannelConfigInput in;
        in.timeout_ms = cfg.recv_timeout_ms;
        in.url = cfg.https_url;              // 通用 url 槽(https 取用)
        in.wss_url = cfg.wss_url;            // wss 专属槽(D9: 默认空 → 永不入选); 由 wss 描述符取用
        in.upload_url = cfg.https_upload_url;
        in.dns_server = cfg.dns_server;
        in.dns_domain = cfg.dns_domain;
        in.icmp_target = cfg.icmp_target;
        return in;
    }

    /**
     * @brief SetConfig channels 规格 → 通道配置输入(P1 收敛: 取代 BuildChannelConfigFromSpec)
     * @details 与旧实现等价: 只搬字段, url 组装交给描述符的 from_config。
     */
    inline ChannelConfigInput FromChannelSpec(const ChannelSpec& spec, uint32_t timeout_ms) {
        ChannelConfigInput in;
        in.host = spec.host;
        in.port = spec.port;
        in.url = spec.url;
        in.upload_url = spec.upload_url;
        in.dns_server = spec.dns_server;
        in.dns_domain = spec.dns_domain;
        in.icmp_target = spec.icmp_target;
        in.timeout_ms = timeout_ms;
        return in;
    }

    // ========================================================================
    // 候选链物化(唯一实现) —— SetupCommsLayer / ReloadChannels 共用
    // ========================================================================

    /**
     * @brief 通道依赖包唯一构造点(装配层): 生产路径填真工厂
     * @details 不预建 —— 未选中的通道不该占用平台资源。与 实测结论一致:
     *          四传输的构造函数可注入 ops, 而 HTTPS/ICMP 的**工厂**不接受 ops,
     *          故依赖包以可替换工厂函数承载, 夹具可换成假 ops。
     *          裁剪形态(CLEAT_NO_QUIC)下 CreateQuicOps 返回 nullptr, 描述符 build
     *          的二级判空会兜住 —— 此处无条件填工厂, 无需宏区分。
     */
    inline ChannelDeps BuildChannelDeps(INetworkOps* net_ops) {
        ChannelDeps deps;
        deps.net_ops = net_ops;
        deps.make_http = []() -> std::unique_ptr<IHttpOps> { return CreateHttpOps(); };
        deps.make_icmp = []() -> std::unique_ptr<IIcmpOps> { return CreateIcmpOps(); };
        deps.make_pipe = []() -> std::unique_ptr<IPipeOps> { return CreatePipeOps(); };
        deps.make_quic = []() -> std::unique_ptr<IQuicOps> { return CreateQuicOps(); };
        return deps;
    }

    /// 物化输入: 一条待物化的通道(type + 已归一的配置输入) —— 来源=启动扁平字段/热切换 spec
    struct ChannelMaterializeInput {
        std::string type;
        ChannelConfigInput in;
    };

    /**
     * @brief 物化策略: 启动装配与热切换的**全部可观察差异**收在这里, 其余行为强制同源
     * @details
     *  - record_unregistered: "未注册/无构造器"是否记入 failed 集。
     *      启动=false: order 来自注册表自身, 未注册项是内部错误, 不属于探测三态;
     *      热=true: spec 由远端下发, 未注册 = "服务端下发了本二进制不支持的类型", 须上报。
     *  - warn_valid_skip: valid 未过(配置不足)是否打 WARN。
     *      启动=false: 未配置是常态(防刷屏), 可见性由快照三态承载;
     *      热=true: 远端显式下发的链, 每一项跳过都值得运营看见。
     *  - warn_config_fail: from_config 失败是否打 WARN(两路径都记 failed)。
     *      启动=false: 失败原因随快照上行, 启动期不逐条刷日志; 热=true。
     */
    struct ChannelMaterializePolicy {
        bool record_unregistered;
        bool warn_valid_skip;
        bool warn_config_fail;
    };

    /// 启动装配策略(SetupCommsLayer): 安静启动, 快照承载可见性
    inline constexpr ChannelMaterializePolicy kMaterializeBootstrapPolicy{ false, false, false };
    /// 热切换策略(ReloadChannels): 远端下发的每一项都 WARN + 记账
    inline constexpr ChannelMaterializePolicy kMaterializeReloadPolicy{ true, true, true };

    /// 物化结果: 候选链 + 能力快照的两个中间量(顺序 = 输入顺序 = 候选优先级)
    struct ChannelMaterializeResult {
        ChannelCandidates candidates;
        std::vector<std::string> caps_active;
        std::vector<std::pair<std::string, std::string>> caps_failed;
    };

    /**
     * @brief 候选链物化(**唯一实现**): 输入集合 → 候选链 + 失败原因集
     * @details 每项: 查描述符 → valid 校验 → from_config 构造 → Create 建实例 →
     *          记录(log/failed/active)。SetupCommsLayer(启动)与 ReloadChannels
     *          (热切换)都调用本函数, 自身只做"输入来源归一"; 差异仅 MaterializePolicy。
     *          此前两路径各持一份手写副本, 因不对称出过 P4 事故(热切换漏填
     *          make_quic → QUIC 通道静默消失) —— 本函数是那次事故的结构性根治。
     * @param registry 装配层注册表(须含本轮注册的全部内建通道)
     * @param deps     依赖包(须由 BuildChannelDeps 构造 —— 唯一来源)
     * @param inputs   已归一的输入集合(顺序 = 候选优先级)
     */
    inline ChannelMaterializeResult MaterializeChannels(
        const ChannelRegistry& registry,
        ChannelDeps& deps,
        const std::vector<ChannelMaterializeInput>& inputs,
        const ChannelMaterializePolicy& policy) {
        ChannelMaterializeResult r;
        for (const auto& item : inputs) {
            const ChannelDescriptor* desc = registry.Descriptor(item.type);
            if (!desc || !desc->from_config) {
                LOG_WARN("AgentContext: 通道[" << item.type << "] 未注册或无可用的配置构造器, 跳过");
                if (policy.record_unregistered) {
                    r.caps_failed.emplace_back(item.type, "unregistered or no config builder");
                }
                continue;
            }
            if (desc->valid && !desc->valid(item.in)) {
                if (policy.warn_valid_skip) {
                    LOG_WARN("AgentContext: 通道[" << item.type << "] 跳过: 配置字段不足");
                }
                continue;   // 未配置(如未给 dns_server/domain)→ 跳过且不记 failed, 交给快照三态
            }
            TransportConfig c;
            std::string why;
            if (!desc->from_config(item.in, c, why)) {
                if (policy.warn_config_fail) {
                    LOG_WARN("AgentContext: 通道[" << item.type << "] 跳过: " << why);
                }
                r.caps_failed.emplace_back(item.type, why);
                continue;
            }
            auto channel = registry.Create(item.type, c, deps);
            if (!channel) {
                LOG_WARN("AgentContext: 通道[" << item.type << "] 构建失败(平台不支持或构建期裁剪)");
                r.caps_failed.emplace_back(item.type, "build failed (platform unsupported or build-time trimmed)");
                continue;
            }
            // 入链日志(两路径同源): bandwidth 供运营核对档位; host 空则不显示
            LOG_INFO("AgentContext: 通道[" << item.type << "] 已加入候选"
                << " bandwidth=" << ToString(channel->Caps().bandwidth)
                << (c.host.empty() ? "" : " host=" + c.host));
            r.caps_active.push_back(item.type);
            r.candidates.emplace_back(std::move(channel), std::move(c));
        }
        return r;
    }

    /**
     * @brief 通道能力快照组装(装配层专用; 服务端能力感知的数据源)
     * @details 三态语义(填"服务端盲发通道配置"的控制面暗坑):
     *   * active        —— 本轮成功入链(顺序 = 候选优先级), 带 bandwidth(服务端
     *                      下发决策所需最小面; 端点细节留 GetConfig, 两面分离)
     *   * unavailable   —— 本轮显式尝试过但没入链(failed 原因透传), 或探测构建失败
     *                      (平台工厂缺失 / 构建期裁剪 —— 裁剪形态的 quic 落此态)
     *   * not_configured—— 注册了且平台支持, 但本轮未配置/未下发(探测构建确认;
     *                      服务端可据此区分"支持但没配"与"根本不支持")
     * 探测构建 = Create 空配置(构建器忽略 cfg), 产物未 Open 即析构 —— 零资源占用
     * (MsQuicOps 构造仅 PIMPL, msquic 资源在 Handshake 才开)。
     * 依赖方向: 纯函数, 只读装配层已持有的 registry/deps —— Handler 侧经
     * AgentConfig 快照消费, 不触碰本函数, 网络依赖不出装配层。
     * @param registry        装配循环用的注册表(须含本轮注册的全部内建通道)
     * @param bootstrap_order registry.BootstrapOrder 的返回(注册表全序)
     * @param active_types    本轮成功入链的通道(按候选优先级)
     * @param failed          本轮显式尝试但未入链的 (type, reason)
     * @param deps            依赖包(探测构建用; 须与装配同源 —— make_quic 已填)
     */
    inline nlohmann::json BuildChannelCapsSnapshot(
        const ChannelRegistry& registry,
        const std::vector<std::string>& bootstrap_order,
        const std::vector<std::string>& active_types,
        const std::vector<std::pair<std::string, std::string>>& failed,
        ChannelDeps& deps) {
        nlohmann::json arr = nlohmann::json::array();
        auto rank_of = [&registry](const std::string& t) -> int {
            const ChannelDescriptor* d = registry.Descriptor(t);
            return d ? d->bootstrap_rank : 0;
            };

        // 1) active: 顺序即优先级; caps 只透出 bandwidth
        for (const auto& t : active_types) {
            nlohmann::json item = { {"type", t}, {"state", "active"} };
            item["rank"] = rank_of(t);
            if (const ChannelDescriptor* d = registry.Descriptor(t); d && d->caps) {
                item["bandwidth"] = ToString(d->caps().bandwidth);
            }
            arr.push_back(std::move(item));
        }
        // 2) 本轮尝试过但未入链: reason 透传(配置物化失败/构建失败)
        for (const auto& [t, why] : failed) {
            nlohmann::json item = { {"type", t}, {"state", "unavailable"}, {"reason", why} };
            item["rank"] = rank_of(t);
            arr.push_back(std::move(item));
        }
        // 3) 其余注册通道: 探测构建定态(not_configured / unavailable)
        for (const auto& t : bootstrap_order) {
            if (std::find(active_types.begin(), active_types.end(), t) != active_types.end()) continue;
            if (std::find_if(failed.begin(), failed.end(),
                [&](const std::pair<std::string, std::string>& p) { return p.first == t; }) != failed.end()) continue;

            // 工厂存在性预判(SelfCheck 同构): 缺工厂的 Create 必然 nullptr, 归因更准
            bool factory_missing = false;
            switch (ChannelDepKindFor(t)) {
            case ChannelDepKind::NetOps: factory_missing = !deps.net_ops; break;
            case ChannelDepKind::Http:   factory_missing = !deps.make_http; break;
            case ChannelDepKind::Icmp:   factory_missing = !deps.make_icmp; break;
            case ChannelDepKind::Pipe:   factory_missing = !deps.make_pipe; break;
            case ChannelDepKind::Quic:   factory_missing = !deps.make_quic; break;
            case ChannelDepKind::None:   break;
            }

            auto probe = registry.Create(t, TransportConfig{}, deps);
            if (probe) {
                arr.push_back({ {"type", t}, {"state", "not_configured"}, {"rank", rank_of(t)} });
            }
            else {
                nlohmann::json item = { {"type", t}, {"state", "unavailable"},
                    {"reason", factory_missing ? "platform factory missing"
                                               : "probe build failed (platform unsupported or build-time trimmed)"} };
                item["rank"] = rank_of(t);
                if (const ChannelDescriptor* d = registry.Descriptor(t); d && !d->note.empty()) {
                    item["note"] = d->note;
                }
                arr.push_back(std::move(item));
            }
        }
        return arr;
    }

} // namespace agentctx_int
} // namespace cleat
