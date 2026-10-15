/**
 * @file IMachineIdOps.hpp
 * @brief 本机身份源采集接口(身份重构②) —— 平台细节全部在平台实现内。
 * @details
 * 契约: CollectSources 按**平台优先级**返回"读到的、非空的"原始源字符串;
 * MachineIdentity 只消费该列表(取第一个 → SHA256(源+盐) 前 16B hex),
 * 不感知源是注册表/文件/固件( 分层, 与 IHttpOps 同款)。
 *
 * 优先级:
 *   Win:   MachineGuid > 卷序列号 > SMBIOS UUID
 *   Linux: /etc/machine-id > /var/lib/dbus/machine-id > DMI product_uuid
 * 全部读不到(概率极低)由 MachineIdentity 回退 hostname 哈希(接受不稳定 ——
 * 落地持久化文件是"捡了芝麻丢了西瓜", 已裁定不做)。
 */
#pragma once

#include <string>
#include <vector>

namespace cleat {
namespace identity {

    class IMachineIdOps {
    public:
        virtual ~IMachineIdOps() = default;

        /// 按平台优先级返回候选源(读到的、非空、已按优先级排序)
        virtual std::vector<std::string> CollectSources() = 0;

        /// 主机名(全源失败的哈希回退输入)
        virtual std::string HostnameFallback() = 0;
    };

} // namespace identity
} // namespace cleat
