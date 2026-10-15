/**
 * @file ChannelRegistry.cpp
 * @brief 通道注册表实现 + 内建通道描述符注册(P1)
 */

#include "Core/Comms/Channel/ChannelRegistry.hpp"

#include <algorithm>
#include <sstream>

#include "Common/Logger/Logger.hpp"
#include "Core/Config/ChannelFieldRules.hpp"
#include "Core/Comms/Transports/Dns/DnsTransport.hpp"
#include "Core/Comms/Transports/Https/HttpsTransport.hpp"
#include "Core/Comms/Transports/Icmp/IcmpTransport.hpp"
#include "Core/Comms/Transports/Tcp/TcpTransport.hpp"
#include "Core/Comms/Transports/Smb/PipeChannel.hpp"
#include "Core/Comms/Transports/WebSocket/WsTransport.hpp"
#include "Core/Comms/Transports/Quic/QuicTransport.hpp"

namespace cleat {

    namespace {

        /// https/wss 通用的 url 组装: 带 scheme 直接用; 仅路径时用 host[:port] 打底。
        /// scheme 由调用方给定(https vs wss), 避免两份 URL 拼装真相。
        std::string BuildHttpLikeUrl(const std::string& raw, const std::string& host,
            uint16_t port, const char* scheme) {
            const std::string sc = scheme;
            if (raw.rfind(sc + "://", 0) == 0) return raw;
            if (host.empty()) return "";
            std::string out = sc + "://" + host;
            if (port != 0 && !(sc == "wss" && port == 443)) out += ":" + std::to_string(port);
            if (!raw.empty()) out += (raw[0] == '/') ? raw : ("/" + raw);
            return out;
        }

        /// pipe(SMB) 的配置构造: pipe_key 直填(空 = 不认证)
        bool BuildPipeConfig(const ChannelConfigInput& in, TransportConfig& out, std::string&) {
            out.pipe_key = in.pipe_key;
            return true;   // pipe 无端点概念, 口令可选
        }

        /// 输入 → 规则表字段子集(P2: 描述符 valid 与 ChannelSpec::Valid 共用同一判据)
        /// @details type 已知时把 wss 的专属槽合并进 url, 使 wss 的判据与其余流式通道同源。
        ChannelFields ToFields(const ChannelConfigInput& in, const std::string& type) {
            ChannelFields f;
            f.host = in.host;
            f.url = (type == "wss" && !in.wss_url.empty()) ? in.wss_url : in.url;
            f.dns_server = in.dns_server;
            f.dns_domain = in.dns_domain;
            f.icmp_target = in.icmp_target;
            return f;
        }

        /// 按类型产出描述符 valid —— 转调单一真相规则表(加通道只在 ChannelFieldRules.hpp 改)
        std::function<bool(const ChannelConfigInput&)> ValidFor(const char* type) {
            const std::string t = type;
            return [t](const ChannelConfigInput& in) {
                return ChannelFieldsSatisfied(t, ToFields(in, t));
                };
        }

    } // namespace

    /**
     * @brief 能力表单一数据源(P2.5): 字符串键 → 能力
     * @details 从旧 `ChannelConfig.cpp::CapsForType(TransportType)` 的 switch 整体搬来,
     *          只把"枚举分派"换成"字符串分派" —— 于是新通道声明能力**不再需要**改
     *          `TransportType` 枚举(枚举退化为 `ITransport::Type()` 的兼容标识)。
     *          各 case 的数值与旧 switch 逐条一致。
     */
    ChannelCaps CapsForTypeStr(const std::string& type) {
        ChannelCaps c;
        if (type == "tcp") {
            c.kind = ChannelKind::Stream;
            c.agency = ChannelAgency::Push;
            c.bandwidth = BandwidthClass::High;
            c.maxFrameBytes = 50 * 1024 * 1024;      // 与 TcpTransport 的 ring 上限一致
        }
        else if (type == "https") {
            c.kind = ChannelKind::Transaction;
            c.agency = ChannelAgency::Poll;          // 服务端只能 enqueue, Agent 来取
            c.bandwidth = BandwidthClass::High;
            c.maxFrameBytes = 32 * 1024 * 1024;      // 与 D9 的响应体上限一致
        }
        else if (type == "dns") {
            c.kind = ChannelKind::Transaction;
            c.agency = ChannelAgency::Poll;
            c.bandwidth = BandwidthClass::Low;
            c.maxFrameBytes = 110 * 255;             // 单分片 110B × 255 片上限()
            c.maxDatagramBytes = 1024;
            c.maxQueuedUplinkBytes = 8 * 1024;       //  以字节为主()
            c.maxQueuedUplinkFrags = 64;
        }
        else if (type == "icmp") {
            c.kind = ChannelKind::Transaction;
            c.agency = ChannelAgency::Poll;
            c.bandwidth = BandwidthClass::Low;
            c.maxFrameBytes = 1400 * 255;            // 单分片 1400B × 255 片上限
            c.maxDatagramBytes = 2048;
            c.maxQueuedUplinkBytes = 179 * 1024;     // ICMP 单分片 1400B → 128 片约 179KB
            c.maxQueuedUplinkFrags = 128;
        }
        else if (type == "pipe") {
            c.kind = ChannelKind::Stream;
            c.agency = ChannelAgency::Push;          // 管道可自发写(对端常驻读)
            c.bandwidth = BandwidthClass::High;      // 本机/内网命名管道, 视作高带宽
            c.maxFrameBytes = 4 * 1024 * 1024;       // 与 PipeChannel 帧上限一致
        }
        else if (type == "wss") {
            // 与 TCP 全同的流式语义(WebSocket 消息边界即帧边界, 无需 4B 定界但保留
            // 会话载荷 4B 头 —— 见 WsWireCodec); 伪装形态 = 普通 Web 应用长连接。
            c.kind = ChannelKind::Stream;
            c.agency = ChannelAgency::Push;          // 服务端可推流(runLoop 现成路径)
            c.bandwidth = BandwidthClass::High;
            c.maxFrameBytes = 50 * 1024 * 1024;      // 对齐 protocol.MaxBodySize(TCP 家族口径)
        }
        else if (type == "quic") {
            // 骨架先按 TCP 同族语义声明: QUIC 连接内单 stream 承载帧流,
            // 与 TCP 同为"字节流需自定界"; 伪装形态 = UDP/443 上的 HTTP/3 流量。
            // 协议栈接入后若按库的 stream 边界改变定界口径, 在此联动。
            c.kind = ChannelKind::Stream;
            c.agency = ChannelAgency::Push;          // 服务端可推流
            c.bandwidth = BandwidthClass::High;
            c.maxFrameBytes = 50 * 1024 * 1024;      // 对齐 protocol.MaxBodySize(TCP 家族口径)
        }
        // 未知类型: 落到 ChannelCaps 默认值(Stream/Push/High), 由 SelfCheck 兜底报出
        return c;
    }

    namespace {

        /// 描述符 caps: 转调字符串能力表(加通道只在本文件的 CapsForTypeStr 加分支)
        std::function<ChannelCaps()> CapsOf(const char* type) {
            const std::string t = type;
            return [t] { return CapsForTypeStr(t); };
        }

    } // namespace

    ChannelRegistry& ChannelRegistry::Instance() {
        static ChannelRegistry instance;
        return instance;
    }

    void ChannelRegistry::Register(const std::string& type, ChannelDescriptor descriptor) {
        if (type.empty() || !descriptor.build) {
            LOG_WARN("ChannelRegistry: 拒绝注册空类型或空构建器");
            return;
        }
        if (descriptors_.find(type) == descriptors_.end()) {
            order_.push_back(type);
        }
        descriptors_[type] = std::move(descriptor);
    }

    void ChannelRegistry::Register(const std::string& type, ChannelBuilder builder) {
        ChannelDescriptor d;
        d.build = std::move(builder);
        // 兼容旧式: caps/from_config/valid 留空 —— SelfCheck 会报为待迁移。
        Register(type, std::move(d));
    }

    bool ChannelRegistry::Has(const std::string& type) const {
        return descriptors_.find(type) != descriptors_.end();
    }

    std::vector<std::string> ChannelRegistry::Types() const {
        return order_;
    }

    const ChannelDescriptor* ChannelRegistry::Descriptor(const std::string& type) const {
        const auto it = descriptors_.find(type);
        return it == descriptors_.end() ? nullptr : &it->second;
    }

    std::unique_ptr<IChannel> ChannelRegistry::Create(const std::string& type,
        const TransportConfig& cfg, ChannelDeps& deps) const {
        const auto it = descriptors_.find(type);
        if (it == descriptors_.end() || !it->second.build) {
            LOG_WARN("ChannelRegistry: 未注册的通道类型 type=" << type);
            return nullptr;
        }
        return it->second.build(cfg, deps);
    }

    std::vector<std::string> ChannelRegistry::BootstrapOrder(const std::string& mode) const {
        // P1 平权: 所有可启动通道**一视同仁** —— 不再为任何通道做
        // "mode 命中即独占/砍掉兄弟"的特判。"只跑 wss"(单候选不混链)这类策略改由
        // **配置**表达(只配 wss_url), 而非写死在装配代码里; "未配置 wss 的产物无 WS 行为"
        //
        // mode 的语义 = **首选通道**(不是"唯一通道"):
        //   - mode 命中某可启动通道 → 它提到最前(rank 提前), 其余仍按 rank 作为**降级候选**;
        //   - mode 为空/未命中 → 纯按 rank 排序。
        // 这样 `-transport wss` 既尊重操作员意图, 又保留 wss 挂掉后降级到 tcp/https 的能力。
        struct Item { std::string type; int rank; size_t seq; };
        std::vector<Item> items;
        for (size_t i = 0; i < order_.size(); ++i) {
            const auto it = descriptors_.find(order_[i]);
            if (it == descriptors_.end()) continue;
            if (!it->second.bootstrap_capable) continue;
            items.push_back({ order_[i], it->second.bootstrap_rank, i });
        }
        std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
            if (a.rank != b.rank) return a.rank < b.rank;
            return a.seq < b.seq;
            });

        std::vector<std::string> result;
        result.reserve(items.size());
        for (const auto& x : items) result.push_back(x.type);

        // 首选通道提前到首位(仅当 mode 命中一个可启动通道时)。
        const auto m = descriptors_.find(mode);
        if (m != descriptors_.end() && m->second.bootstrap_capable) {
            auto pos = std::find(result.begin(), result.end(), mode);
            if (pos != result.end() && pos != result.begin()) {
                std::rotate(result.begin(), pos, pos + 1);
            }
        }
        return result;
    }

    std::vector<std::string> ChannelRegistry::SelfCheck(const ChannelDeps* deps) const {
        std::vector<std::string> issues;
        for (const auto& type : order_) {
            const auto it = descriptors_.find(type);
            if (it == descriptors_.end()) continue;
            const ChannelDescriptor& d = it->second;

            if (!d.caps) {
                issues.push_back("通道[" + type + "] 缺 caps(旧式注册未迁移)");
            }
            if (!d.from_config) {
                issues.push_back("通道[" + type + "] 缺 from_config(旧式注册未迁移)");
            }
            if (!d.valid) {
                issues.push_back("通道[" + type + "] 缺 valid 判据(旧式注册未迁移)");
            }
            // pipe 是唯一的已知豁免: 它由 PipeSessionHub 自建(非主链候选), 故
            // bootstrap_capable=false 属预期。其余任何通道若为 false, 都是"注册成功
            // 但启动链永不选中"的静默失效风险, 必须报出。
            const bool known_exempt = (type == "pipe");
            if (!d.bootstrap_capable && !known_exempt) {
                issues.push_back("通道[" + type + "] bootstrap_capable=false 且非豁免 —— "
                    "注册成功但启动链永不选中(静默失效风险)");
            }
            if (d.needs_dep && deps) {
                bool missing = false;
                switch (ChannelDepKindFor(type)) {
                case ChannelDepKind::NetOps: missing = !deps->net_ops; break;
                case ChannelDepKind::Http:   missing = !deps->make_http; break;
                case ChannelDepKind::Icmp:   missing = !deps->make_icmp; break;
                case ChannelDepKind::Pipe:   missing = !deps->make_pipe; break;
                case ChannelDepKind::Quic:   missing = !deps->make_quic; break;
                case ChannelDepKind::None:   break;
                }
                if (missing) {
                    issues.push_back("通道[" + type + "] needs_dep=true 但当前平台未提供工厂"
                        "(构建将返回 nullptr)");
                }
            }
        }
        return issues;
    }

    std::vector<std::string> ChannelRegistry::DumpDescriptors() const {
        std::vector<std::string> lines;
        for (const auto& type : order_) {
            const auto it = descriptors_.find(type);
            if (it == descriptors_.end()) continue;
            const ChannelDescriptor& d = it->second;
            std::ostringstream os;
            os << type
                << " bandwidth=" << (d.caps ? ToString(d.caps().bandwidth) : "?")
                << " bootstrap=" << (d.bootstrap_capable ? "yes" : "no")
                << " rank=" << d.bootstrap_rank
                << (d.note.empty() ? "" : (" (" + d.note + ")"));
            lines.push_back(os.str());
        }
        return lines;
    }

    void RegisterBuiltinChannels(ChannelRegistry& registry) {
        // ---- tcp: 流式/推送/高带宽; 无额外端点(Chain 连接阶段注入 host/port) ----
        {
            ChannelDescriptor d;
            d.build = [](const TransportConfig&, ChannelDeps& deps) -> std::unique_ptr<IChannel> {
                if (!deps.net_ops) return nullptr;
                return std::make_unique<TcpTransport>(*deps.net_ops);
                };
            d.caps = CapsOf("tcp");
            d.from_config = [](const ChannelConfigInput& in, TransportConfig& out, std::string&) {
                out.host = in.host;      // 空 → 由 Chain 在连接阶段注入启动 host/port
                out.port = in.port;
                return true;
                };
            d.valid = ValidFor("tcp");   // host 空合法(规则表: tcp 恒真)
            d.bootstrap_capable = true;
            d.bootstrap_rank = 10;
            d.needs_dep = true;
            d.note = "Stream/Push";
            registry.Register("tcp", std::move(d));
        }

        // ---- https: 事务/轮询/高带宽 ----
        {
            ChannelDescriptor d;
            d.build = [](const TransportConfig&, ChannelDeps& deps) -> std::unique_ptr<IChannel> {
                if (!deps.make_http) return nullptr;   // 该平台未实现
                return std::make_unique<HttpsTransport>(deps.make_http());
                };
            d.caps = CapsOf("https");
            d.from_config = [](const ChannelConfigInput& in, TransportConfig& out, std::string& why) {
                out.url = BuildHttpLikeUrl(in.url, in.host, in.port, "https");
                if (out.url.empty()) { why = "缺少可解析 URL"; return false; }
                if (!in.upload_url.empty()) {
                    out.upload_url = BuildHttpLikeUrl(in.upload_url, in.host, in.port, "https");
                }
                return true;
                };
            d.valid = ValidFor("https");
            d.bootstrap_capable = true;
            d.bootstrap_rank = 20;
            d.needs_dep = true;
            d.note = "Transaction/Poll";
            registry.Register("https", std::move(d));
        }

        // ---- dns: 事务/轮询/低带宽 ----
        {
            ChannelDescriptor d;
            d.build = [](const TransportConfig&, ChannelDeps& deps) -> std::unique_ptr<IChannel> {
                if (!deps.net_ops) return nullptr;
                return std::make_unique<DnsTransport>(*deps.net_ops);
                };
            d.caps = CapsOf("dns");
            d.from_config = [](const ChannelConfigInput& in, TransportConfig& out, std::string& why) {
                if (in.dns_server.empty() || in.dns_domain.empty()) {
                    why = "缺少 dns_server/dns_domain";
                    return false;
                }
                out.dns_server = in.dns_server;
                out.domain = in.dns_domain;
                return true;
                };
            d.valid = ValidFor("dns");
            d.bootstrap_capable = true;
            d.bootstrap_rank = 30;
            d.needs_dep = true;
            d.note = "Transaction/Poll/Low";
            registry.Register("dns", std::move(d));
        }

        // ---- icmp: 事务/轮询/低带宽 ----
        {
            ChannelDescriptor d;
            d.build = [](const TransportConfig&, ChannelDeps& deps) -> std::unique_ptr<IChannel> {
                if (!deps.make_icmp) return nullptr;   // 该平台未实现
                return std::make_unique<IcmpTransport>(deps.make_icmp());
                };
            d.caps = CapsOf("icmp");
            d.from_config = [](const ChannelConfigInput& in, TransportConfig& out, std::string& why) {
                if (in.icmp_target.empty()) { why = "缺少 icmp_target"; return false; }
                out.icmp_target = in.icmp_target;
                return true;
                };
            d.valid = ValidFor("icmp");
            d.bootstrap_capable = true;
            d.bootstrap_rank = 40;
            d.needs_dep = true;
            d.note = "Transaction/Poll/Low";
            registry.Register("icmp", std::move(d));
        }

        // ---- pipe(SMB): Pivot 内部通道; 由 PipeSessionHub 自建, 不参与主链 ----
        {
            ChannelDescriptor d;
            d.build = [](const TransportConfig&, ChannelDeps& deps) -> std::unique_ptr<IChannel> {
                if (!deps.make_pipe) return nullptr;   // 该平台未实现
                return std::make_unique<PipeChannel>(deps.make_pipe());
                };
            d.caps = CapsOf("pipe");
            d.from_config = BuildPipeConfig;
            d.valid = ValidFor("pipe");   // 口令可选(规则表: pipe 恒真)
            d.bootstrap_capable = false;   // Pivot 自建(PipeSessionHub), 不属主链候选
            d.bootstrap_rank = -1;
            d.needs_dep = true;
            d.note = "Pivot-internal (self-built, not a main-chain candidate)";
            registry.Register("pipe", std::move(d));
        }

        // ---- wss: 流式/推送/高带宽; **与 tcp/https 平权**(可降级/可设置) ----
        {
            ChannelDescriptor d;
            d.build = [](const TransportConfig&, ChannelDeps& deps) -> std::unique_ptr<IChannel> {
                if (!deps.net_ops) return nullptr;
                auto ops = CreateWssOps(*deps.net_ops);
                if (!ops) return nullptr;   // 平台未实现(工厂按条件编译, 缺失即 nullptr)
                return std::make_unique<WsTransport>(std::move(ops));
                };
            d.caps = CapsOf("wss");
            d.from_config = [](const ChannelConfigInput& in, TransportConfig& out, std::string& why) {
                // P2: wss 的 url 槽选择属"通道专属逻辑", 就地取 wss_url(空则退回通用 url);
                // 装配层因此无需 `type == "wss"` 特判, 只做纯字段搬运。
                const std::string raw = in.wss_url.empty() ? in.url : in.wss_url;
                if (raw.rfind("wss://", 0) == 0) {
                    out.url = raw;
                }
                else {
                    out.url = BuildHttpLikeUrl(raw, in.host, in.port, "wss");
                }
                if (out.url.empty()) { why = "缺少可解析 URL"; return false; }
                return true;
                };
            d.valid = ValidFor("wss");
            d.bootstrap_capable = true;    // 平权: 与 tcp/https 一样可进启动链
            d.bootstrap_rank = 50;         // 兜底序: 排在 tcp/https/dns/icmp 之后
            d.needs_dep = true;
            d.note = "Stream/Push";
            registry.Register("wss", std::move(d));
        }

        // ---- quic: 流式/推送/高带宽; msquic 满血实现 ----
        // 显式配 `--url quic://...` 或热切换 spec(quic:// URL 或 host/port)后进入候选。
        {
            ChannelDescriptor d;
            d.build = [](const TransportConfig&, ChannelDeps& deps) -> std::unique_ptr<IChannel> {
                if (!deps.make_quic) return nullptr;   // 该平台未实现(如 Linux 侧构建未接入)
                auto ops = deps.make_quic();
                if (!ops) return nullptr;
                return std::make_unique<QuicTransport>(std::move(ops));
                };
            d.caps = CapsOf("quic");
            d.from_config = [](const ChannelConfigInput& in, TransportConfig& out, std::string& why) {
                // 端点来源(与判据同源): 通用 url 槽的 quic:// URL, 或显式 host(+port)。
                const std::string raw = in.url;
                if (raw.rfind("quic://", 0) == 0) {
                    out.url = raw;
                }
                else if (!in.host.empty()) {
                    out.url = "quic://" + in.host;
                    if (in.port != 0) out.url += ":" + std::to_string(in.port);
                }
                if (out.url.empty()) { why = "缺少 quic:// URL 或显式 host"; return false; }
                return true;
                };
            d.valid = ValidFor("quic");
            d.bootstrap_capable = true;
            d.bootstrap_rank = 60;         // 稳定序垫底; 需要提权时调整(与 tcp=10/wss=50 同表)
            d.needs_dep = true;
#ifdef CLEAT_NO_QUIC
            d.note = "Stream/Push (构建期裁剪: CLEAT_WITH_QUIC=OFF)";
#else
            d.note = "Stream/Push (msquic, OpenSSL backend)";
#endif
            registry.Register("quic", std::move(d));
        }

        LOG_INFO("ChannelRegistry: 内建通道已注册 count=" << registry.Types().size());
    }

} // namespace cleat
