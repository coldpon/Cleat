/**
 * @file IDeadDrop.hpp
 * @brief  死投递平台适配器统一接口
 * @details
 * 职责分离: 适配器**只负责"怎么从该平台取/发原始内容"**(爬虫/写端),
 * 验签/序列号/解密在 DeadDropCrypto(Manager 统一持有) —— 加新平台 =
 * Platform/<名>/ 建目录写适配器 + DeadDropPlatforms 注册一行 + 配置加一条,
 * 内容层逻辑一行不动()。
 *
 * 读/写两面(死投递=只读恢复, 活投递=低频命令收发):
 *   - FetchRaw            下行(所有平台必备) —— 恢复与活投递取件共用;
 *   - Publish/CanPublish  上行(活投递平台实现, 如 Telegram/Discord Webhook);
 *                           只读平台(Gist 匿名)不覆写 = 如实上报不支持,
 *                           **不得**让"发布"静默成功假象存在。
 *
 * 适配器经 IHttpOps 发请求(headers 传平台 UA, 见 IHttpOps.hpp 4.0.1),
 * 不感知加密与会话 —— 它是通道层的**调用者**, 不是通道。
 */
#pragma once

#include <optional>
#include <string>

namespace cleat {
namespace deaddrop {

    class IDeadDrop {
    public:
        virtual ~IDeadDrop() = default;

        /**
         * @brief 拉取平台原始内容(含 DD1 信封行的完整文本)
         * @return 平台不可达/网络失败 = std::nullopt(交 Manager 尝试下一平台);
         *         内容取到(哪怕无信封行/已被删帖) = 原文交由 Manager 校验
         */
        virtual std::optional<std::string> FetchRaw() = 0;

        /**
         * @brief 发布内容到平台(活投递上行预留)
         * @return true = 已发布; false = 不支持/失败(调用方区分: CanPublish()==false
         *               时不必尝试)。内容格式由调用方组好(DD1 信封), 适配器只投递。
         */
        virtual bool Publish(const std::string& /*content*/) { return false; }

        /// 该平台是否具备上行能力(只读平台 false —— 管理层据此跳过发布尝试)
        virtual bool CanPublish() const { return false; }

        /// 平台标识(日志/调试/注册表键)
        virtual std::string PlatformName() const = 0;
    };

} // namespace deaddrop
} // namespace cleat
