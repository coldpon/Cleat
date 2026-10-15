/**
 * @file FrameClassifier.cpp
 * @brief 帧分类实现
 * @details 判别次序即契约(见头文件的"顺序不变量"), 搬移时**保持逐条一致**,
 *          并把每个分支的成立条件写清楚 —— 顺序与条件的正确性由用例守护。
 */

#include "Core/Comms/Session/FrameClassifier.hpp"

#include <algorithm>
#include <optional>

namespace cleat {

    namespace {
        /**
         * (第十七轮): session_id 的**唯一**提取口径, SessionUnknown 与 SessionConfirm
         * 两个分支共用。旧实现(SessionUnknown 分支)只认 `is_number_unsigned()` ——
         * 服务端以有符号整数/字符串/浮点下发 sid 时, 分类结果静默退化为 0, 归属校验
         * 被跳过(与 SessionConfirm 的 signed 兼容分支口径漂移)。
         *
         * 返回值三态:
         *   * nullopt  = 字段**存在但非法**(字符串/浮点/负数/bool 等; sid 恒为正, 负数
         *                同样属畸形) —— 调用方裁定: SessionUnknown 帧按 Ignored 处理
         *                (帧不可信, 不得参与归属判定), Confirm 帧按 sid=0 处理
         *                (状态机本就拒绝非法确认);
         *   * 0        = 字段不存在(由调用方先判), 或值为 0(消费方既有语义:
         *                "无从路由, 照常受理");
         *   * >0       = 提取成功(无符号原样; 有符号仅收非负)。
         */
        std::optional<uint64_t> ExtractSessionId(const nlohmann::json& v) {
            if (v.is_number_unsigned()) {
                return v.get<uint64_t>();
            }
            if (v.is_number_integer()) {
                const int64_t s = v.get<int64_t>();
                if (s < 0) return std::nullopt;   // 负 sid = 畸形()
                return static_cast<uint64_t>(s);
            }
            return std::nullopt;   // 存在但非法()
        }
    } // namespace

    ClassifiedFrame ClassifyFrame(const std::vector<uint8_t>& payload) {
        ClassifiedFrame out;

        // ---- 0) 分帧形态: 混合帧(JSON头 + '\0' + 二进制体) vs 纯 JSON ----
        // 注意: **不能依赖 parse 异常**检测混合帧 —— nlohmann 会把 '\0' 当输入结束
        // 而成功解析, 导致二进制体丢失。必须主动查找分隔符。
        nlohmann::json req;
        try {
            const auto sep = std::find(payload.begin(), payload.end(), 0);
            if (sep != payload.end() && sep + 1 != payload.end()) {
                req = nlohmann::json::parse(payload.begin(), sep);
                out.binary.assign(sep + 1, payload.end());
            }
            else {
                req = nlohmann::json::parse(payload.begin(), payload.end());
            }
        }
        catch (const std::exception& e) {
            out.kind = FrameKind::Unknown;
            out.parse_error = e.what();
            return out;
        }

        if (!req.is_object()) {
            return out;   // Unknown: 非对象
        }

        const auto it_status = req.find("status");
        const auto it_data = req.find("data");

        // ---- ① 服务端拒绝(防抖 DENY_BUSY / 会话失效 SESSION_UNKNOWN) ----
        // 必须是第一个判别: 会话层据此**立即**按失败处理, 不必等 8s 确认超时
        // (根治重连"假成功"风暴的核心信号)。非这两个码的错误帧仍按"已解析但无动作"处理。
        if (it_status != req.end() && it_status->is_string() &&
            it_status->get<std::string>() == "error") {
            const auto it_code = req.find("code");
            if (it_code != req.end() && it_code->is_string()) {
                out.code = it_code->get<std::string>();
            }
            if (out.code == "DENY_BUSY") {
                out.kind = FrameKind::Denied;
            }
            else if (out.code == "SESSION_UNKNOWN") {
                // 服务端回带它**不认得**的那个 sid(0 = 帧内本无 sid, 服务端无法路由)。
                // 会话层用它做归属校验: 只对"就是自己当前 sid"的失效通知清身份重注册。
                out.kind = FrameKind::SessionUnknown;
                const auto it_sid = req.find("session_id");
                if (it_sid == req.end()) {
                    return out;   // 帧内本无 sid → sid=0(无从路由), 归属判定交给会话层
                }
                const auto sid = ExtractSessionId(*it_sid);
                if (!sid) {
                    //  sid 存在但解析不出(字符串/浮点/负数) → 帧不可信, 按"已解析
                    // 但无动作"处理。**不得**当作 sid=0 受理 —— 那会跳过归属校验, 任何
                    // 把 sid 编码改形的服务端变更(TS 侧 reregisterFrame 的 sid 是 Go int)
                    // 都能让"上一次会话的迟到失效通知"清掉当前身份。
                    out.kind = FrameKind::Ignored;
                    return out;
                }
                out.session_id = *sid;   // 与 SessionConfirm 共用同一口径()
            }
            else {
                out.kind = FrameKind::Ignored;
            }
            return out;
        }

        // ---- ③ FirstConnect 会话确认 ----
        if (it_status != req.end() && it_status->is_string() &&
            it_status->get<std::string>() == "success" &&
            it_data != req.end() && it_data->is_object() &&
            it_data->contains("session_id")) {
            out.kind = FrameKind::SessionConfirm;
            //  与 SESSION_UNKNOWN 共用同一提取口径 —— 两个分支各写一份时口径会漂移
            //(实测漂移正是本条的病根)。malformed(存在但非法)按 sid=0 处理: 状态机对
            // sid=0 的确认本就拒绝("非法"), 身份不会被污染, 与旧行为一致。
            out.session_id = ExtractSessionId((*it_data)["session_id"]).value_or(0);
            return out;
        }

        // ---- ④ 普通命令帧 ----
        const auto it_cmd = req.find("cmd");
        if (it_cmd == req.end() || !it_cmd->is_string()) {
            return out;   // Unknown/Ignored: 无 cmd 且不匹配以上任何形状
        }
        out.kind = FrameKind::Command;
        out.cmd = it_cmd->get<std::string>();

        const auto it_req = req.find("request_id");
        if (it_req != req.end() && it_req->is_string()) {
            out.request_id = it_req->get<std::string>();
        }
        const auto it_params = req.find("params");
        if (it_params != req.end() && it_params->is_object()) {
            out.params = *it_params;
        }
        return out;
    }

} // namespace cleat
