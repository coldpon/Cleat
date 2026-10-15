/**
 * @file MaskRuntime.hpp
 * @brief 睡眠掩码运行时（MaskWindow P1：镜像枚举 + 线程普查 + TimerObf 式窗口执行）
 * @details
 *
 * 定位（P1 交付物）：
 *   - **自测/干跑**：镜像信息、线程普查（谁在跑、谁的 start 在本模块镜像内）、
 *     掩码原语解析与 RC4 往返自检——不执行窗口，任何时刻可跑；
 *   - **窗口执行**（execute=true）：在"全静默"前提下执行一次真实掩码窗口
 *     （整镜像 RC4 加密 → 定时器链过窗 → 解密还原）。调用方必须先保证
 *     进程内不存在"镜像内名单外线程"（本实现会以普查拒绝，属安全不变量）。
 *
 * 依赖纪律：本头文件平台中立（零平台 API，规则 1），平台实现见
 * `PlatformWin_MaskRuntime.cpp` / `PlatformLinux_MaskRuntime.cpp`；
 * **本模块不依赖 Logger**（维持"可独立链接进单线程探针"的性质，
 * Tools/MaskProbe 直接链接本模块的两个源文件即可运行）。
 *
 * 与 Havoc Demon 的对照：窗口执行的链结构对齐其 `Obf.c::TimerObf`
 * （RtlCreateTimer + NtContinue 上下文链 + SystemFunction032 整镜像 RC4），
 * 差异见平台实现文件头注释。
 *
 * ⚠️ 开发者约束（窗口期禁区；改"镜像内代码"前必读）：完整清单见
 * 镜像内代码执行 = 执行密文 = 崩溃（新增静态 TLS 回调、镜像内线程回调、
 * /MT 构建、错实现的通道 In-flight 查询皆会破坏约束）。
 */
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cleat {

    /// 本模块镜像信息（"本模块" = 掩码代码所在模块，EXE/DLL 两形态通用）
    struct MaskImageInfo {
        uint64_t base = 0;        ///< 模块基址
        uint64_t size = 0;        ///< SizeOfImage（含全部节）
        uint32_t regionCount = 0; ///< VirtualQuery 分段数（还原保护用）
    };

    /// 线程普查条目
    struct MaskThreadEntry {
        uint32_t tid = 0;
        uint64_t startAddress = 0; ///< Win32 起始地址（0 = 查询失败）
        bool inOwnImage = false;   ///< start 落入本模块镜像
        bool allowlisted = false;  ///< 在调用方允许名单内
        bool isSelf = false;       ///< 调用线程自身
        int64_t createdDeltaMs = -1; ///< 相对进程启动的创建时刻(诊断定位漏网线程; 不可得 = -1)
        uint64_t ripSnapshot = 0;    ///< 普查瞬时 RIP(诊断: 该线程此刻卡在哪; 0 = 不可得)
        uint64_t ripImageOffset = 0; ///< RIP 落在本镜像内时 = RIP - base(否则 0)
    };

    /// 自测报告
    struct MaskSelfTestReport {
        bool supported = false;        ///< 平台是否支持
        bool executed = false;         ///< 是否执行了真实窗口
        MaskImageInfo image;
        std::vector<MaskThreadEntry> threads;
        int foreignInImageCount = 0;   ///< start∈镜像 且不在允许名单（不含自身）
        int unknownStartCount = 0;     ///< start 查询失败的线程数（仅报告，不阻断）
        // ---- 预检 ----
        bool primitivesOk = false;     ///< 所需导出（ntdll/kernel32/cryptsp）解析齐全
        bool rc4RoundTripOk = false;   ///< SystemFunction032 加解密往返自检
        std::string missingExport;     ///< primitivesOk=false 时首个缺失符号
        // ---- 执行结果（executed=true 时有效） ----
        uint32_t windowMs = 0;         ///< 实际请求的窗口时长
        uint64_t elapsedMs = 0;        ///< 链排队→过窗→解密返回的总耗时
        uint64_t hashBefore = 0;       ///< 窗口前镜像 FNV-1a
        uint64_t hashAfter = 0;        ///< 窗口后镜像 FNV-1a
        bool integrityOk = false;      ///< 密文期写 ≤ 本底容忍(见 windowDiffBytes; 真"全冻结"在活进程不可达)
        /// 完整性失败时的差异诊断（integrityOk=true 时为空）:
        /// { offset 相对镜像基址, bytes 差异字节数 } —— 定位"谁在窗口期写了镜像"
        struct DiffRange {
            uint64_t offset;         ///< 差异区间起点(页粒度)
            uint64_t bytes;          ///< 区间内差异总字节数(页粒度计)
            uint64_t firstDiff = 0;  ///< 首个差异字节(绝对 offset)
            uint64_t beforeHead = 0; ///< 首差异处前 8 字节(快照侧, LE)
            uint64_t afterHead = 0;  ///< 首差异处前 8 字节(窗口后侧, LE)
        };
        std::vector<DiffRange> diffRanges;
        /// 分相差异(定位写手在"明文准备期"还是"密文窗口期"):
        uint64_t preQueueDiffBytes = 0;  ///< 窗口排队前(普查/RC4/建链) vs 快照0
        uint64_t windowDiffBytes = 0;    ///< 排队后(屏障→加密→过窗→解密) vs 排队前
        std::string note;              ///< 说明/降级原因
    };

    /**
     * @brief 睡眠掩码运行时接口
     */
    class IMaskRuntime {
    public:
        virtual ~IMaskRuntime() = default;

        /// 平台支持与否（Linux 恒 false）
        virtual bool Supported() const = 0;

        /**
         * @brief 自测/干跑/窗口执行
         * @param execute   false = 干跑（镜像 + 普查 + 原语预检）；true = 执行一次真实窗口
         * @param windowMs  窗口时长，execute=true 时有效（建议 ≥1000；实现内做下限保护）
         * @param allowTids 允许名单线程 tid（调用方登记其常驻线程；自身隐式允许）
         * @param out       报告（supported 字段在返回前总是被填）
         * @param err       未完成原因
         * @return true = 自测完成（无论干跑还是执行，报告有效）；
         *         false = **自测未完成**（唯一语义；具体失败点看 err）
         * @details execute=true 时若普查发现"镜像内名单外线程" → 拒绝执行并返回 false
         *          （安全不变量：窗口内不允许任何镜像内线程存活于名单外）。
         */
        virtual bool SelfTest(bool execute, uint32_t windowMs,
            const std::vector<uint32_t>& allowTids,
            MaskSelfTestReport& out, std::string& err) = 0;
    };

    /// 工厂：平台实现各定义一份（PlatformWin_/PlatformLinux_）
    std::unique_ptr<IMaskRuntime> CreateMaskRuntime();

} // namespace cleat
