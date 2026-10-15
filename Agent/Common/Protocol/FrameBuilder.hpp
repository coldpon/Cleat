#pragma once
#include "Common/ThirdParty/json.hpp"
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace cleat {

    class FrameBuilder {

    private:
        /// 进程级状态: `FrameBuilder` 是纯静态工具类(全仓构造点直接调 `Build`,
        /// 无实例可持状态), 故 provider 只能挂在类静态成员上。
        static std::function<uint64_t()> session_id_provider_;

    public:
		static uint64_t NowSeconds() {
			return static_cast<uint64_t>(
				std::chrono::duration_cast<std::chrono::seconds>(
					std::chrono::system_clock::now().time_since_epoch()).count());
		}

        /**
         * @brief 注入 session_id 提供者(装配根接线一次)
         * @details
         * **为什么走注入而不是直接读 `ClientContext`**: 本文件在 `Common/` 层, 而
         * `ClientContext` 在 `Core/` 层 —— 直接 include 会让依赖方向倒置(`Common` 反
         * 依赖 `Core`), 且让本工具类无法脱离核心单测。故与 `AgentConfig` 同法,
         * 由装配根(`AgentContext::Initialize`)**接线一次**。
         *
         * **为什么信封注入放在这里**: 若放开手工注入, `session_id` 会散落在顶层 /
         * `data` / `params` 三种位置各写各的; 回调透传链路(Provider → dataCallback_
         * → send)每一层都可能漏, 导致流数据帧不带 `session_id`, 服务端把每一帧都
         * 按"非注册首帧"做匿名注册 —— 一帧一个新会话(实测一次远程桌面可开出数十个
         * 匿名会话)。
         *
         * 信封字段与 `ts` 同源治理: `ts` 在 `Build` 中一处注入(全仓构造点无人手工塞),
         * 本字段此前没有享受同等待遇 —— 差别仅在"`ts` 是纯时间函数, 而 `session_id`
         * 是运行时状态", 需要的正是这个 provider 回调。
         */
        static void SetSessionIdProvider(std::function<uint64_t()> provider) {
            session_id_provider_ = std::move(provider);
        }

        static std::vector<uint8_t> Build(const nlohmann::json& payload) {
            nlohmann::json j = payload;
            j["ts"] = NowSeconds();
            InjectSessionId(j);
            const std::string s = j.dump(-1, ' ', false,
                                         nlohmann::json::error_handler_t::replace);
            return std::vector<uint8_t>(s.begin(), s.end());
        }
        /**
         * @brief 混合帧构造(唯一形态): JSON头 + '\0' + 二进制体
         * @details
         * body_size **恒以实际体长写入**(头里手写的任何值都被覆盖) —— 与 TeamServer
         * 的帧构造保持同契约, 构造权威唯一(本类), 杜绝同协议两套构造行为漂移。
         * 单参重载即"空体混合帧"(线上为裸 JSON, 无分隔符) —— 两形态共享同一概念模型。
         */
        static std::vector<uint8_t> Build(const nlohmann::json& header,
                                          const std::vector<uint8_t>& body) {
            nlohmann::json h = header;
            h["ts"] = NowSeconds();
            InjectSessionId(h);
            h["body_size"] = body.size();
            const std::string s = h.dump(-1, ' ', false,
                                         nlohmann::json::error_handler_t::replace);
            std::vector<uint8_t> out(s.begin(), s.end());
            out.push_back('\0');
            out.insert(out.end(), body.begin(), body.end());
            return out;
        }

    private:
        /**
         * @brief 信封注入: 顶层写 `session_id`(已存在则不覆盖)
         * @details
         * **顶层而非 `data`/`params`**: 服务端 `frameSessionID` 的查找次序是
         * "顶层 → `params` → `data`", 顶层优先级最高; 放顶层对两类帧(响应 / 主动推送)
         * 都成立, 不必区分它们各自把业务载荷放在哪个段。
         *
         * **不覆盖已有值**: 调用方若已显式指定, 那是**更精确的意图**, 不应被通用值盖掉。
         *
         * **`sid == 0` 时不注入**: 未注册期没有会话身份, 写入 0 只会制造
         * "看起来有值、其实是空"的误导帧 —— 服务端两种情况的处理本来就一样
         * (都按无 sid 走), 但前者会让排障者浪费一次排查。
         */
        static void InjectSessionId(nlohmann::json& j) {
            if (j.contains("session_id")) return;   // 调用方已显式指定, 尊重之
            if (!session_id_provider_) return;
            const uint64_t sid = session_id_provider_();
            if (sid != 0) {
                j["session_id"] = sid;
            }
        }
    };

    /// 类静态成员的定义(C++17 前需类外定义; 显式给出以免依赖 inline 变量)
    inline std::function<uint64_t()> FrameBuilder::session_id_provider_{};

} // namespace cleat
