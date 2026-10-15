/**
 * @file ChannelConfig.cpp
 * @brief 通道能力表实现(类型 → 能力)
 */

#include "Core/Comms/Channel/ChannelConfig.hpp"

#include "Core/Comms/Channel/ChannelRegistry.hpp"   // CapsForTypeStr(能力表单一数据源)

namespace cleat {

    /**
     * @brief 枚举 → 能力(兼容薄层)
     * @details P2.5 起能力表的**单一数据源**是 `CapsForTypeStr(字符串)`(见 ChannelRegistry.cpp),
     *          本函数退化为"枚举→字符串→查表"的转发, 供 `ITransport::Caps()` 的默认实现
     *          与既有夹具继续使用。**能力的声明请勿写在此处**(只在注册表声明)。
     *          新增枚举值时(如 Quic)在此补一行**转发 case** —— 转发不引入第二份能力
     *          真相; /W4 的 C4062(switch 未穷举)会点名提醒补上。
     */
    ChannelCaps CapsForType(TransportType type) {
        switch (type) {
        case TransportType::Tcp:   return CapsForTypeStr("tcp");
        case TransportType::Https: return CapsForTypeStr("https");
        case TransportType::Dns:   return CapsForTypeStr("dns");
        case TransportType::Icmp:  return CapsForTypeStr("icmp");
        case TransportType::Smb:   return CapsForTypeStr("pipe");
        case TransportType::Ws:    return CapsForTypeStr("wss");
        case TransportType::Quic:  return CapsForTypeStr("quic");
        }
        return ChannelCaps{};
    }

} // namespace cleat
