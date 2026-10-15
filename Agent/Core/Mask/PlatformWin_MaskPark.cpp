/**
 * @file PlatformWin_MaskPark.cpp
 * @brief 停靠协议 —— Windows 实现(epoch 门控 ack + 手动复位放行事件)
 * @details
 *
 * 关键取舍:
 *   - **手动复位事件**做放行闸: 多参与者等待一次 Set 全部释放(auto-reset 只放一个)。
 *     每轮 Enter 先 ResetEvent —— 上一轮迟醒的参与者会错过放行而滞留到下一轮放行,
 *     期间它持续停靠, 安全; 它没 ack 本轮 → 本轮 Enter 超时放弃(fail-closed)。
 *   - **ack 无 epoch 门控的简化**: 参与者一次 ack 后立即进入无超时等待, 醒来且
 *     "无新一轮请求"才退出检查点 —— 每个等待周期至多一次 ack, 同一轮不会被重复计数;
 *     上一轮迟到的 ack 落在新轮 `acks=0` 之前或之后都只会让计数**多不少**,
 *     Enter 只要求"收齐 expected"(≥), 多者无副作用。
 *   - **窗口互斥**(g_active): 双发起者(如 dormant 主循环与自测命令并发)会让第二者的
 *     ResetEvent 打进第一者的放行闸 —— 后到者直接 Enter 失败。
 */
#include "Core/Mask/MaskPark.hpp"

#ifdef CLEAT_WITH_MASK

#include <windows.h>

#include <atomic>

namespace cleat {
namespace MaskPark {
namespace {

    /// 参与者 tid 槽位(0 = 空; 上限 8 —— 参与者仅 worker/reaper/main 三条)
    constexpr size_t kSlotCount = 8;
    std::atomic<uint32_t> g_slots[kSlotCount] = {};

    std::atomic<bool> g_requested{ false };     ///< 停靠请求(发起者写, 参与者读)
    std::atomic<uint32_t> g_acks{ 0 };          ///< 本轮已停靠确认数(每轮 Enter 清零)
    std::atomic<uint32_t> g_active{ 0 };        ///< 窗口互斥(0/1; 双发起者防护)
    std::atomic<void*> g_release{ nullptr };    ///< 手动复位放行事件(惰性创建, 永不关闭)
    std::atomic<bool> g_notify_unsafe{ false }; ///< DLL 线程通知污染(taint; 见头文件)

    HANDLE ReleaseEvent() {
        void* ev = g_release.load(std::memory_order_acquire);
        if (ev) {
            return static_cast<HANDLE>(ev);
        }
        HANDLE fresh = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        void* expected = nullptr;
        if (g_release.compare_exchange_strong(expected, fresh)) {
            return fresh;
        }
        // 别的线程先建好了: 丢弃自己的, 用它的
        if (fresh) {
            ::CloseHandle(fresh);
        }
        return static_cast<HANDLE>(g_release.load(std::memory_order_acquire));
    }

    /// 放行(唯一两个调用点: Enter 放弃路径 / 会话析构成功路径)。请求先撤, 事件后 Set
    /// —— 醒来的参与者复查 requested 即退出, 不会误入下一轮。
    void Leave() {
        g_requested.store(false, std::memory_order_release);
        if (const HANDLE ev = ReleaseEvent()) {
            ::SetEvent(ev);
        }
        g_active.store(0, std::memory_order_release);
    }

} // namespace

    Registration::Registration() {
        const uint32_t self = ::GetCurrentThreadId();
        for (auto& slot : g_slots) {
            uint32_t expected = 0;
            if (slot.compare_exchange_strong(expected, self)) {
                return;
            }
        }
        // 槽位满: 保持未注册 —— 普查按"名单外镜像内线程"拒绝窗口(fail-closed)
    }

    Registration::~Registration() {
        const uint32_t self = ::GetCurrentThreadId();
        for (auto& slot : g_slots) {
            uint32_t expected = self;
            if (slot.compare_exchange_strong(expected, 0)) {
                return;
            }
        }
    }

    std::vector<uint32_t> RegisteredTids() {
        std::vector<uint32_t> out;
        out.reserve(kSlotCount);
        for (const auto& slot : g_slots) {
            if (const uint32_t tid = slot.load(std::memory_order_acquire)) {
                out.push_back(tid);
            }
        }
        return out;
    }

    bool Session::Enter(uint32_t timeoutMs) {
        // 窗口互斥: 已有会话在途(另一发起者) → 本轮放弃
        uint32_t expected = 0;
        if (!g_active.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
            return false;
        }
        const HANDLE ev = ReleaseEvent();
        if (!ev) {
            g_active.store(0, std::memory_order_release);
            return false;
        }

        // 期望 ack 数 = 已注册的"非自身"线程数。快照在置位前取: 参与者名册运行期
        // 只在 worker/reaper/main 出生时增长, 注册晚于快照的线程下一轮才参与。
        const uint32_t self = ::GetCurrentThreadId();
        uint32_t expectedAcks = 0;
        for (const auto& slot : g_slots) {
            const uint32_t tid = slot.load(std::memory_order_acquire);
            if (tid != 0 && tid != self) {
                ++expectedAcks;
            }
        }

        ::ResetEvent(ev);
        g_acks.store(0, std::memory_order_relaxed);
        g_requested.store(true, std::memory_order_release);

        const ULONGLONG deadline = ::GetTickCount64() + timeoutMs;
        while (g_acks.load(std::memory_order_acquire) < expectedAcks) {
            if (::GetTickCount64() >= deadline) {
                Leave();   // 放弃路径: 已 ack 的参与者必须被放行, 不存在"半停靠"
                return false;
            }
            ::Sleep(1);
        }
        entered_ = true;   // 全员停靠; 此后 ~Session 的 Leave 在窗口结束(解密后)执行
        return true;
    }

    Session::~Session() {
        if (entered_) {
            Leave();
        }
    }

    void ParticipantCheckpoint() {
        // 快路径: 无请求一次原子读(空闲常态; 开关 OFF 时本函数整体为空实现)
        if (!g_requested.load(std::memory_order_acquire)) {
            return;
        }        const HANDLE ev = ReleaseEvent();
        if (!ev) {
            // 事件不可得 = Enter 同样拿不到事件 = 不会有窗口启动; 不停靠, 照常执行
            return;
        }
        for (;;) {
            if (!g_requested.load(std::memory_order_acquire)) {
                return;
            }
            g_acks.fetch_add(1, std::memory_order_acq_rel);   // ack 本轮停靠
            // 无超时等待(设计): 窗口期醒一拍 = 执行密文。放行必然发生
            // —— Leave 只有两个调用点, 且都紧随"请求撤除"。
            ::WaitForSingleObject(ev, INFINITE);
            if (!g_requested.load(std::memory_order_acquire)) {
                return;
            }
            // 醒来但新一轮已在途(错过上一轮放行) → 继续停靠并 ack 新一轮
        }
    }

    void MarkThreadNotifyUnsafe() {
        g_notify_unsafe.store(true, std::memory_order_release);
    }

    bool ThreadNotifyUnsafe() {
        return g_notify_unsafe.load(std::memory_order_acquire);
    }

    bool DetectTlsCallbacks() {
        // 解析"本函数所在模块"的 PE 头: TLS 目录的**回调数组**是否非空。
        // 只有回调(加载器直接调用、DLTLC 抑制不了)才构成窗口期密文执行。
        // 异常布局一律按"有回调"处理(fail-closed)。
        HMODULE mod = nullptr;
        if (!::GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&DetectTlsCallbacks), &mod) || !mod) {
            return true;
        }
        const uint8_t* base = reinterpret_cast<const uint8_t*>(mod);
        auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
            return true;
        }
        const uint8_t* nt = base + dos->e_lfanew;
        if (*reinterpret_cast<const uint32_t*>(nt) != IMAGE_NT_SIGNATURE) {
            return true;
        }
        auto* opt32 = reinterpret_cast<const IMAGE_OPTIONAL_HEADER64*>(nt + 24);
        if (opt32->Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
            return true;
        }
        const IMAGE_DATA_DIRECTORY& tls = opt32->DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
        if (tls.VirtualAddress == 0 || tls.Size == 0) {
            return false;   // 无 TLS 目录 = 无回调
        }
        // IMAGE_TLS_DIRECTORY64: +0x18 = AddressOfCallBacks(首选基址基准的 VA)
        const uint8_t* dir = base + tls.VirtualAddress;
        const uint64_t cbVa = *reinterpret_cast<const uint64_t*>(dir + 0x18);
        if (cbVa == 0) {
            return false;
        }
        const uint64_t cbRva = cbVa - opt32->ImageBase;
        if (cbRva >= opt32->SizeOfImage) {
            return true;   // 异常布局: 按有回调处理
        }
        const uint64_t* entries = reinterpret_cast<const uint64_t*>(base + cbRva);
        for (size_t i = 0; i < 64; ++i) {
            if (entries[i] == 0) {
                return false;   // NULL 终止 = 无回调
            }
        }
        return true;   // 64 项仍未终止: 视为有回调
    }

} // namespace MaskPark
} // namespace cleat

#endif // CLEAT_WITH_MASK
