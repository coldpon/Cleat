/**
 * @file EventReporter.cpp
 * @brief 日志事件上报器实现(从 AgentContext 原样迁移, 行为保持清单见头文件)
 *
 * 限速策略:
 *   1. 单条节流: 按 (source:line + message) 做 10s 窗口去重;
 *      节流表上限 512, 满时先清过期项, 仍满则淘汰最旧一条(不再整体 clear)。
 *   2. 全局令牌桶: ERROR 与其余事件各一个桶, 稳态 2/s + 5 突发 与 5/s + 10 突发。
 *   3. 有界队列: 256 条上限, 满则丢弃新事件(容量检查与 push 同锁)。
 *   4. Drain 分批: 每轮最多 32 条, 抑制单轮突发。
 *   5. 发送失败即丢弃剩余(EventReport 为低价值告警, 不重试)。
 */
#include "EventReporter.hpp"

#include "Common/Logger/Logger.hpp"
#include "Common/ThirdParty/json.hpp"
#include "Common/Protocol/FrameBuilder.hpp"
#include "Core/Comms/OutboundMessages.hpp"
#include "Core/Identity/SessionIdentity.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace cleat {
    namespace {

        /// 递归入 sink 防护: 日志线程正持有网络锁(Flush 内 POST 失败路径)时,
        /// 上报自身触发的日志不得再进入上报管线(实测 0xE06D7363 resource_deadlock)。
        /// **TLS 已禁**(DLL 形态掩码前提, ): thread_local → 互斥
        /// set<线程id>, 语义逐字保留 —— 同线程重入被拒, 跨线程照常上报。
        std::mutex& SinkGuardMutex() {
            static std::mutex m;
            return m;
        }
        std::set<std::thread::id>& SinkGuardInside() {
            static std::set<std::thread::id> s;
            return s;
        }
        /// 返回 true = 本线程已在 sink 内(重入, 调用方丢弃本次上报)
        bool SinkGuardEnter() {
            std::lock_guard<std::mutex> lock(SinkGuardMutex());
            return !SinkGuardInside().insert(std::this_thread::get_id()).second;
        }
        void SinkGuardExit() {
            std::lock_guard<std::mutex> lock(SinkGuardMutex());
            SinkGuardInside().erase(std::this_thread::get_id());
        }

        std::string LevelName(LogLevel lv) {
            switch (lv) {
            case LogLevel::__DEBUG: return "DEBUG";
            case LogLevel::INFO:    return "INFO";
            case LogLevel::WARN:    return "WARN";
            case LogLevel::_ERROR:  return "ERROR";
            default:                return "UNKNOWN";
            }
        }

        // ------------------------------------------------------------------
        // 全局令牌桶: 按毫秒线性补充, 上限 burst_, 稳态速率 rate_/s。
        // ------------------------------------------------------------------
        class TokenBucket {
        public:
            TokenBucket(int64_t rate_per_sec, int64_t burst)
                : rate_(rate_per_sec),
                burst_(burst),
                tokens_(burst),
                last_(std::chrono::steady_clock::now()) {
            }

            bool TryConsume() {
                std::lock_guard<std::mutex> lock(mutex_);
                const auto now = std::chrono::steady_clock::now();
                const auto delta_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - last_).count();
                if (delta_ms > 0) {
                    tokens_ = std::min(burst_, tokens_ + delta_ms * rate_ / 1000);
                    last_ = now;
                }
                if (tokens_ <= 0) return false;
                --tokens_;
                return true;
            }

        private:
            int64_t rate_;
            int64_t burst_;
            int64_t tokens_;
            std::chrono::steady_clock::time_point last_;
            std::mutex mutex_;
        };

        // ERROR: 稳态 2/s, 突发 5; 其余: 稳态 5/s, 突发 10。
        // 需要更松/更紧时, 只改这两个构造参数即可。
        TokenBucket g_errorBucket(2, 5);
        TokenBucket g_normalBucket(5, 10);

        bool AllowByGlobalRate(LogLevel lv) {
            if (lv == LogLevel::_ERROR) return g_errorBucket.TryConsume();
            return g_normalBucket.TryConsume();
        }

        // ------------------------------------------------------------------
        // 单条节流: (key, ts_ms) 10s 窗口去重, 表有界。
        // ------------------------------------------------------------------
        constexpr int64_t kThrottleWindowMs = 10'000;
        constexpr size_t  kThrottleMaxEntries = 512;

        /// 表满时按时间淘汰最旧条目, 而不是整体清空 —— 否则高基数 key
        /// 会周期性打穿节流。
        bool AllowByPerKeyThrottle(const std::string& key, int64_t ts_ms) {
            static std::mutex throttle_mutex;
            static std::map<std::string, int64_t> last_sent;

            std::lock_guard<std::mutex> lock(throttle_mutex);
            auto it = last_sent.find(key);
            if (it != last_sent.end() && ts_ms - it->second < kThrottleWindowMs) {
                return false;
            }

            if (last_sent.size() >= kThrottleMaxEntries) {
                // 先清过期项。
                for (auto m = last_sent.begin(); m != last_sent.end();) {
                    if (ts_ms - m->second >= kThrottleWindowMs) {
                        m = last_sent.erase(m);
                    }
                    else {
                        ++m;
                    }
                }
                // 仍满: 淘汰时间戳最旧的一条, 给新 key 让位。
                if (last_sent.size() >= kThrottleMaxEntries) {
                    auto oldest = last_sent.begin();
                    for (auto m = last_sent.begin(); m != last_sent.end(); ++m) {
                        if (m->second < oldest->second) oldest = m;
                    }
                    last_sent.erase(oldest);
                }
            }

            last_sent[key] = ts_ms;
            return true;
        }

        constexpr size_t kQueueMax = 256;
        constexpr size_t kDrainBatchMax = 32;

    } // namespace

    void EventReporter::SetSender(Sender sender) {
        // AD9(修复 ): 与读侧(HandleEventReport/Drain)同锁。当前只在装配期
        // 调用一次, 但把"仅装配期"的时序假设落到代码上 —— 将来支持运行期重绑时
        // 不会与日志线程/主循环产生 std::function 撕裂读。
        std::lock_guard<std::mutex> lock(mutex_);
        sender_ = std::move(sender);
        senderSet_ = true;
    }

    void EventReporter::Install() {
        Logger::Instance().SetEventSink([this](const EventRecord& rec) {
            HandleEventReport(rec);
            });
        installed_ = true;
    }

    EventReporter::~EventReporter() {
        // 对称注销: 见头文件说明。此刻 Logger 必然存活 ——
        // 它先于本类(AgentContext 栈对象成员)构造、晚于本类析构, Instance() 不会
        // 重建单例, 故本调用安全; installed_ 判定避免"未 Install 却清掉他人 sink"。
        if (!installed_) return;
        Logger::Instance().SetEventSink(nullptr);
        installed_ = false;
    }

    void EventReporter::HandleEventReport(const EventRecord& rec) {
        if (SinkGuardEnter()) return;
        struct SinkGuard { ~SinkGuard() { SinkGuardExit(); } } sink_guard;

        try {
            // source 标识: 文件名:行号
            std::string src = rec.file;
            auto slash = src.find_last_of("/\\");
            if (slash != std::string::npos) src = src.substr(slash + 1);
            src += ":" + std::to_string(rec.line);
            const std::string key = src + rec.message;

            // 1) 单条 10s 节流
            if (!AllowByPerKeyThrottle(key, rec.ts_ms)) return;

            // 2) 全局限速(ERROR 与其余分开)
            if (!AllowByGlobalRate(rec.level)) return;

            // 3) 队列容量预检(避免队列满时白做 JSON 构造)
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!senderSet_ || queue_.size() >= kQueueMax) return;
            }

            // 组包收口(M1): 帧 shape({"cmd","data"})与 SendKind 声明收口到 msg::EventReport。
            // data.ts(毫秒)与信封顶层 ts(秒)撞名不同义 —— data 段被服务端整帧转发
            // 前端/落盘, 本批不改名(已知妥协, 见 OutboundMessages 文件头)。
            const auto out = msg::EventReport({
                {"session_id", SessionIdentity::Instance().GetSessionId()},
                {"level", LevelName(rec.level)},
                {"source", src},
                {"message", rec.message},
                {"ts", rec.ts_ms}
                });

            const std::string json_str = out.payload.dump(-1, ' ', false,
                nlohmann::json::error_handler_t::replace);

            // 4) 入队: 容量检查与 push 同锁, 避免 TOCTOU 超发。
            //    只入队, 由主循环 Drain 统一发送(同步 Send 会递归锁死锁, 见文件头)。
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!senderSet_ || queue_.size() >= kQueueMax) return;
                queue_.push_back(std::vector<uint8_t>(json_str.begin(), json_str.end()));
            }
        }
        catch (...) {
            // 静默丢弃，避免递归
        }
    }

    void EventReporter::Drain() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.empty() || !senderSet_) return;
        }
        const uint64_t sid = SessionIdentity::Instance().GetSessionId();
        if (sid == 0) {
            // 未注册: 上报无会话可路由, server 会把无身份帧当匿名首帧注册
            // (实测 EventReport 匿名风暴), 直接丢弃整批。
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.clear();
            return;
        }

        // 每轮最多发 kDrainBatchMax 条, 抑制单轮突发;
        // 剩余留在队列里, 由主循环下一轮继续 Drain。
        std::deque<std::vector<uint8_t>> pending;
        Sender sender;   // AD9: 锁内拷一份 —— 发送在锁外(不得持锁调 sink), 但读 sender_ 必须同锁
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!senderSet_) return;
            sender = sender_;
            const size_t n = std::min(queue_.size(), kDrainBatchMax);
            for (size_t i = 0; i < n; ++i) {
                pending.push_back(std::move(queue_.front()));
                queue_.pop_front();
            }
        }

        if (!sender) return;   // AD9: 锁内取到的那份为空(理论上不可达: 上面已判 senderSet_)

        // 发送失败(断线/积压): 丢弃剩余(EventReport 为低价值告警, 不重试)
        for (auto& frame : pending) {
            // 发送时绑定当前会话: 帧可能在注册前入队(当时 sid=0), 若按入队
            // 时的 sid 发出, server 会当匿名首帧 → 匿名注册风暴(实测)。
            try {
                nlohmann::json j = nlohmann::json::parse(frame.begin(), frame.end());
                j["data"]["session_id"] = sid;
                frame = FrameBuilder::Build(j);
            }
            catch (...) {
                continue;   // 解析失败: 非标准帧, 丢弃
            }
            if (!sender(frame)) break;
        }
    }

} // namespace cleat