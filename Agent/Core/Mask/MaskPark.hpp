#pragma once
/**
 * @file MaskPark.hpp
 * @brief 睡眠掩码停靠协议
 * @details
 * 掩码窗口内**任何镜像内代码执行 = 执行密文 = 崩溃**, 故窗口期间除发起者(停在内核等待)
 * 外, 所有"我方常驻线程"必须停靠在**不会自己醒来的内核等待**上。本协议覆盖
 * cmd-worker / cmd-reaper 两条常驻线程, 并把 session-main 泛化为
 * "参与者"(当发起者是自检命令的 job 线程时, 主循环经 `SleepMs` 分片检查点停靠)。
 *
 * ⚠️ 窗口期禁区(改"镜像内代码"前必读; 违反 = 掩码窗口期崩溃):
 *   - 窗口加密期间(整镜像 RC4), **任何镜像内代码执行 = 执行密文 = 崩溃**;
 *   - 禁止新增静态 TLS 回调(thread_local 析构会生成; POST_BUILD 检查会拒绝构建),
 *     数据型 TLS 无害但避免引入;
 *   - 禁止 /MT 构建掩码(CMake FATAL 已拒): UCRT FLS 线程退出回调在镜像内;
 *   - 新常驻线程必须: 经 JobRuntime 统一出生 + 本文件 Registration 入册 +
 *     循环顶 ParticipantCheckpoint(), 否则普查 fail-closed 拒绝窗口(功能静默失效);
 *   - 禁止窗口期可达的镜像内回调: VEH(DbgVeh=ON 构建禁测稳定性)、镜像内钩子过程、
 *     镜像内定时器/线程池回调;
 *   - 新增通道必须正确实现 NextPollDueInMs/PendingUplinkBytes(IChannel) —— 错实现
 *     的在飞 IO 会让窗口期线程唤醒 → 执行密文。
 *
 * 协议(发起者 = 掩码睡眠的执行线程):
 *   - 参与者在**循环顶部、手上无活**时经 `ParticipantCheckpoint()` 接入:
 *     有停靠请求 → ack → 无超时事件等待(Leave 恒 Set, 不会永眠);
 *   - 发起者 `Session::Enter(timeout)`: 置请求 → 收齐"非自身已注册参与者"的 ack
 *     (收不齐 = 放弃本轮, 内部放行) → 调用方执行掩码窗口 → 析构 `Leave()` 放行;
 *   - **放行时序(安全性核心)**: Leave 只会在镜像解密完成之后发生 —— 成功路径由
 *     ~Session 保证(掩码链末步 SetEvent 之后调用方才返回), 放弃路径镜像从未加密。
 *
 * 线程普查与停靠的关系: 普查(MaskRuntime::SelfTest)证"集合成员", ack 证"停靠状态"
 * —— 名单内线程若没到检查点, ack 收不齐, Enter 失败, 本轮放弃(fail-closed)。
 *
 * 编译分态: `CLEAT_WITH_MASK` 未定义时本头提供**空实现**(调用点一次原子读的开销),
 * 循环接入点(CommandWorker/SessionLoop)因此无需 #ifdef; 真实现见
 * `PlatformWin_MaskPark.cpp` / `PlatformLinux_MaskPark.cpp`, 仅在 ON 时编入
 * (Agent/CMakeLists.txt 按开关裁剪 Core/Mask 整目录)。
 *
 * **平凡可析构**对象 + 惰性创建永不关闭的内核事件 —— 掩码窗口不会发生在静态析构期,
 * 但停靠等待不允许依赖任何"可能已析构"的锁或单例。
 */
#include <cstdint>
#include <vector>

namespace cleat {
namespace MaskPark {

#ifdef CLEAT_WITH_MASK

    /**
     * @brief 参与者注册(线程自身 RAII 入册; tid 槽位表上限 8)
     * @details 槽位满 = 工程性错误(参与者只有 worker/reaper/main 三条), 保持未注册
     *          **fail-closed**: 普查把未注册的镜像内线程视为名单外, 窗口会被拒绝。
     */
    class Registration {
    public:
        Registration();
        ~Registration();
        Registration(const Registration&) = delete;
        Registration& operator=(const Registration&) = delete;
    };

    /// 已注册线程 tid 快照(普查允许名单 = 本表 ∪ 调用线程自身; MaskGate 传入 SelfTest)
    std::vector<uint32_t> RegisteredTids();

    /**
     * @brief 发起者会话: Enter 收齐 ack → 调用方执行掩码窗口 → 析构放行。
     * @details 任何路径(判定失败/链失败/异常)都必须让对象析构 —— 放行是 RAII 兜底,
     *          否则参与者永眠(任何路径都必须走到 Leave)。
     *          窗口互斥: 同一时刻至多一个会话(双发起者防护, 后到者 Enter 直接失败)。
     */
    class Session {
    public:
        Session() = default;
        ~Session();
        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;

        /**
         * @brief 请求全部"非自身"已注册参与者停靠并收 ack
         * @param timeoutMs 收 ack 的上界(主循环节拍 100ms + 执行抖动,
         *                  取 500ms 留余量 —— 超时只延迟放弃, 从不降低安全性)
         * @return false = 本轮放弃(超时/已有窗口/事件创建失败), 调用方走普通睡眠;
         *         true  = 全部参与者已停靠, 可执行窗口
         */
        bool Enter(uint32_t timeoutMs);

    private:
        bool entered_ = false;
    };

    /**
     * @brief 参与者接入点(循环顶部; 手上无活时调用)
     * @details 无请求时一次原子读返回; 有请求 → ack → 无超时等待放行 → 返回后
     *          调用方继续正常循环(镜像已解密)。**等待无超时**: 窗口期醒一拍就是
     *          执行密文, 放行由发起者析构/放弃路径保证必然发生。
     * @note 调用纪律: **发起者不得在持有自己 Session 的同时调用本函数** ——
     *       它会被自己的停靠请求冻住直到析构(自死锁)。生产形态天然满足:
     *       dormant 发起者(session-main)在窗口内不回睡眠分片, 自测命令发起者
     *       (job 线程)不接检查点; 二者身份互斥。
     */
    void ParticipantCheckpoint();

    /**
     * @brief 线程通知/TLS 污染标记
     * @details DLL 形态: 自带静态 TLS(thread_local 变量)时 `DisableThreadLibraryCalls`
     *          必然失败 —— 窗口期任何系统线程生灭都会以 DLL_THREAD_ATTACH/DETACH
     *          回调本镜像的 `_DllMainCRTStartup`(密文) = 崩溃。DllMain 检测到抑制
     *          失败即置位, 掩码窗口对本进程永久禁用(fail-closed)。
     *          EXE 形态: 无线程通知, 但**静态 TLS 回调数组**同样参与线程生灭
     *          (EXE 入口 EntryExe.cpp 调 DetectTlsCallbacks, 命中即置位)。
     *          清除路径: 移除镜像的全部 thread_local/静态 TLS 回调。
     */
    void MarkThreadNotifyUnsafe();
    bool ThreadNotifyUnsafe();

    /// 解析自身 PE 的 TLS 目录, 返回**回调数组是否非空**(不置标记; DllMain 组合
    /// "通知抑制失败 || 本函数" 决定是否 MarkThreadNotifyUnsafe, EXE 入口单用本函数)。
    ///   - **数据型 TLS 无害**: 变量存在时 DisableThreadLibraryCalls 仍返回 TRUE
    ///     (通知成功抑制, 最小探针 DLL 实测); 数据槽由加载器清零, 无代码执行。
    ///     UCRT 的 _Init_thread_epoch 即此类(/MT 与 /MD 都有)。
    ///   - **回调致命**: 加载器在线程生灭时**直接调用**回调(该 API 抑制不了)。
    ///   - **/MT 的 FLS 陷阱**: /MT 下 UCRT 本体在镜像内, 其 FLS 线程退出回调
    ///     同样在镜像内执行 —— 即使回调数组为空也不安全(Release 真机复现崩溃)。
    ///   - **/MD 解锁**: UCRT 移入系统 ucrtbase.dll(镜像外), 回调数组为空 +
    ///     通知(仅 DLL 有此项)抑制成功 = 条件全齐 → 不污染, 掩码解锁。
    /// DllMain(PROCESS_ATTACH) 与 EXE 入口(EntryExe.cpp)两端调用(EXE 同判据:
    /// "EXE 不收线程通知"仅对 DLL_THREAD_ATTACH 成立, TLS/FLS 回调路径 EXE 同样中招)。
    bool DetectTlsCallbacks();

#else  // !CLEAT_WITH_MASK —— 空实现: 调用点零改动, 链接面闭合

    class Registration {
    public:
        Registration() {}
    };

    inline std::vector<uint32_t> RegisteredTids() { return {}; }

    class Session {
    public:
        bool Enter(uint32_t) { return false; }
    };

    inline void ParticipantCheckpoint() {}
    inline void MarkThreadNotifyUnsafe() {}
    inline bool ThreadNotifyUnsafe() { return false; }
    inline bool DetectTlsCallbacks() { return false; }

#endif // CLEAT_WITH_MASK

} // namespace MaskPark
} // namespace cleat
