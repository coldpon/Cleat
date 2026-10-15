/**
 * @file SleepMaskProbeHandler.cpp
 * @brief 睡眠掩码自测命令实现(仅 CLEAT_WITH_MASK=ON 有执行体)
 * @details
 * 验收语义(设计 P1): 输出线程普查快照、镜像哈希(前/后)、窗口耗时、降级原因。
 * execute 路径: TestPrecheck(静默, 自身豁免) → MaskPark 停靠(其余常驻线程) →
 * MaskRuntime::SelfTest(execute=true, 普查兜底 + 真窗口) → 放行(RAII)。
 * 长窗口(> worker 超时 60s)下发时操作员须带 `timeout_ms` 放宽本任务超时
 * (CommandProcessor 通用机制, 同 BofExec); 即便超时"遗弃"也只是响应丢失,
 * 窗口线程本身 detach 继续, RAII 保证镜像最终还原。
 */
#include "SleepMaskProbeHandler.hpp"

#include "Core/Config/JsonField.hpp"
#include "Core/Mask/MaskGate.hpp"
#include "Core/Mask/MaskPark.hpp"
#include "Core/Mask/MaskRuntime.hpp"
#include "Common/Protocol/Packet.hpp"

#ifdef CLEAT_WITH_MASK

#include <windows.h>

#include <algorithm>
#include <memory>
#include <vector>

namespace cleat {
namespace {

    /// execute 模式窗口下限保护(与 MaskRuntime kMinWindowMs 一致的命令侧钳位)
    constexpr uint32_t kMinWindowMs = 500;
    constexpr uint32_t kMaxWindowMs = 120000;   ///< 设计 建议上限(120s)

    /// 自测报告 → JSON(验收语义 P1: 普查快照/哈希前后/窗口耗时/降级原因)
    nlohmann::json ReportToJson(const MaskSelfTestReport& r) {
        nlohmann::json j;
        j["supported"] = r.supported;
        j["executed"] = r.executed;
        j["image"] = {
            {"base", r.image.base},
            {"size", r.image.size},
            {"regions", r.image.regionCount},
        };
        nlohmann::json arr = nlohmann::json::array();
        for (const MaskThreadEntry& t : r.threads) {
            arr.push_back({
                {"tid", t.tid},
                {"start", t.startAddress},
                {"in_image", t.inOwnImage},
                {"allow", t.allowlisted},
                {"self", t.isSelf},
                {"born_ms", t.createdDeltaMs},
                {"rip", t.ripSnapshot},
                {"rip_img_off", t.ripImageOffset},
                });
        }
        j["threads"] = std::move(arr);
        j["foreign_in_image"] = r.foreignInImageCount;
        j["unknown_start"] = r.unknownStartCount;
        j["primitives_ok"] = r.primitivesOk;
        j["rc4_roundtrip_ok"] = r.rc4RoundTripOk;
        if (!r.missingExport.empty()) {
            j["missing_export"] = r.missingExport;
        }
        j["window_ms"] = r.windowMs;
        j["elapsed_ms"] = r.elapsedMs;
        j["hash_before"] = r.hashBefore;
        j["hash_after"] = r.hashAfter;
        j["integrity_ok"] = r.integrityOk;
        nlohmann::json diffs = nlohmann::json::array();
        for (const auto& d : r.diffRanges) {
            diffs.push_back({ {"offset", d.offset}, {"bytes", d.bytes},
                {"first_diff", d.firstDiff},
                {"before_head", d.beforeHead}, {"after_head", d.afterHead} });
        }
        j["diff_ranges"] = std::move(diffs);
        j["pre_queue_diff_bytes"] = r.preQueueDiffBytes;
        j["window_diff_bytes"] = r.windowDiffBytes;
        j["note"] = r.note;
        return j;
    }

} // namespace

    nlohmann::json SleepMaskProbeHandler::Execute(const CommandRequest& req) {
        bool execute = false;
        detail::ReadBool(req.params, "execute", execute);
        uint32_t windowMs = 3000;
        detail::ReadUInt(req.params, "window_ms", windowMs);
        windowMs = (std::min)((std::max)(windowMs, kMinWindowMs), kMaxWindowMs);

        std::unique_ptr<IMaskRuntime> rt = CreateMaskRuntime();
        if (!rt || !rt->Supported()) {
            return Packet::Error("SleepMaskProbe: 平台不支持睡眠掩码");
        }

        // 空闲抖动探针(不执行窗口): 双快照比对, 判定"窗口期镜像写"是窗口造成的
        // 还是系统本底(若空闲期同样抖动, 完整性检查需按本底折算)。
        bool churn = false;
        detail::ReadBool(req.params, "churn_probe", churn);
        if (churn) {
            MaskSelfTestReport pre;
            std::string perr;
            if (!rt->SelfTest(false, 0, std::vector<uint32_t>{}, pre, perr)) {
                return Packet::Error(std::string("churn: 干跑未完成(") + perr + ")");
            }
            const size_t n = static_cast<size_t>(pre.image.size);
            std::vector<uint8_t> a(n, 0), b(n, 0);
            std::memcpy(a.data(), reinterpret_cast<const void*>(pre.image.base), n);
            ::Sleep(5000);
            std::memcpy(b.data(), reinterpret_cast<const void*>(pre.image.base), n);
            uint64_t diffBytes = 0;
            for (size_t i = 0; i < n; ++i) {
                diffBytes += (a[i] != b[i]) ? 1 : 0;
            }
            // 首个差异页(诊断粒度与窗口报告一致)
            nlohmann::json ranges = nlohmann::json::array();
            constexpr size_t kPage = 4096;
            bool inRange = false;
            uint64_t start = 0, bytes = 0, firstDiff = 0;
            for (size_t off = 0; off < n; off += kPage) {
                const size_t m = (std::min)(kPage, n - off);
                const bool dirty = std::memcmp(a.data() + off, b.data() + off, m) != 0;
                if (dirty) {
                    if (!inRange) { inRange = true; start = off; bytes = 0;
                        for (size_t k = 0; k < m; ++k) {
                            if (a[off + k] != b[off + k]) { firstDiff = off + k; break; }
                        }
                    }
                    bytes += m;
                } else if (inRange) {
                    inRange = false;
                    ranges.push_back({ {"offset", start}, {"bytes", bytes}, {"first_diff", firstDiff} });
                }
            }
            if (inRange) {
                ranges.push_back({ {"offset", start}, {"bytes", bytes}, {"first_diff", firstDiff} });
            }
            return Packet::Ok({ { "churn_diff_bytes", diffBytes }, { "ranges", std::move(ranges) } });
        }

        // execute 先过静默预检(自身豁免) + 停靠; 失败也回干跑报告(普查快照仍有效)
        MaskSelfTestReport rep;
        std::string err;
        if (execute) {
            MaskGate::SilenceParams sp;
            sp.channel_is_poll = true;   // 预检不限定通道(普查会拒绝 Push 通道的接收线程)
            sp.outbound_queued_bytes = 0;
            std::string pre;
            if (!MaskGate::TestPrecheck(sp, pre)) {
                return Packet::Error(std::string("SleepMaskProbe: 静默预检拒绝(") + pre + ")");
            }
            MaskPark::Session park;
            // 2000ms(非 MaskGate 的 500ms): 本命令的发起者是 job 线程, 停靠对象含
            // session-main —— 主循环醒来后的补 check-in(GET+POST)会推迟它的 ack;
            // 稳态掩码开启时 job 与主循环的窗权竞争(互斥后到者等待)也吃这段预算。
            // 放宽只影响"放弃的耐心", 不影响安全性(收不齐照样拒绝进窗)。
            if (!park.Enter(2000)) {
                return Packet::Error("SleepMaskProbe: 停靠收不齐 ack(常驻线程未到检查点/窗口互斥让位), 放弃本轮");
            }
            // SelfTest 内部: 原语/镜像/普查/RC4 → 真窗口; 普查拒绝时镜像未被加密
            const bool done = rt->SelfTest(true, windowMs, MaskPark::RegisteredTids(), rep, err);
            if (!done) {
                rep.supported = true;
                rep.note = "执行未完成";
                nlohmann::json data;
                data["report"] = ReportToJson(rep);
                data["error"] = err;
                return Packet::Ok(std::move(data));
            }
        }
        else {
            if (!rt->SelfTest(false, 0, MaskPark::RegisteredTids(), rep, err)) {
                return Packet::Error(std::string("SleepMaskProbe: 干跑未完成(") + err + ")");
            }
        }
        return Packet::Ok({ { "report", ReportToJson(rep) } });
    }

} // namespace cleat

#else // !CLEAT_WITH_MASK —— 明确错误(命令注册同样裁剪; 本体保留便于链接面稳定)

namespace cleat {

    nlohmann::json SleepMaskProbeHandler::Execute(const CommandRequest&) {
        return Packet::Error("SleepMaskProbe: 产物未编入睡眠掩码(需 CLEAT_WITH_MASK=ON 构建)");
    }

} // namespace cleat

#endif // CLEAT_WITH_MASK
