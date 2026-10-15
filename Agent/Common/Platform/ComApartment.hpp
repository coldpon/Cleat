/**
 * @file ComApartment.hpp
 * @brief COM 公寓的按调用点初始化/配对释放（Windows；其他平台空操作）
 * @details
 * 全仓 COM 公寓的**唯一**初始化工具（ 起；框架 guard ThreadInitGuard
 * 已删除）。定位是惰性工具：纯头文件、不进任何框架执行路径、框架代码零调用 ——
 * 谁用 COM，谁在自己的调用点放一个本对象，框架全程不知情。线程出生时
 * （JobRuntime::Spawn 等）一律无 COM 公寓。
 *
 * 判据（原 CameraJob 的 ComGuard / EdrRecon 的 comHere / Persistence 的
 * comInitByUs 各处手写样板收敛于此）：
 *   * 仅当本次调用返回 S_OK 才持有，析构时配对一次 CoUninitialize；
 *   * S_FALSE（线程已有公寓）= 复用现有公寓，不持有、不释放；
 *   * RPC_E_CHANGED_MODE（公寓模式不同，如线程已是 MTA）= 复用，不持有、不释放；
 *   * 其他 FAILED 同样不持有；可用 hr() 区分，调用方自行决定是否提前退出。
 *
 * 公寓模式：STA（COINIT_APARTMENTTHREADED）。需要 MTA 的组件（EdrRecon 的 WMI）
 * 自行调 CoInitializeEx(MTA) 并配对，不要用本类。
 * 约束：本类不泵消息 —— 需要服务入向跨公寓调用的对象不得依赖本类所在线程。
 */

#pragma once

#ifdef _WIN32
#include <windows.h>
#include <combaseapi.h>
#endif

namespace cleat::Platform {

    class ScopedComApartment {
    public:
        ScopedComApartment() {
#ifdef _WIN32
            hr_ = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            owned_ = (hr_ == S_OK);
#endif
        }

        ~ScopedComApartment() {
#ifdef _WIN32
            if (owned_) {
                CoUninitialize();
            }
#endif
        }

        ScopedComApartment(const ScopedComApartment&) = delete;
        ScopedComApartment& operator=(const ScopedComApartment&) = delete;

        /// true = 本次调用真实初始化了公寓，析构时将配对释放；false = 复用现有公寓或初始化失败
        bool owned() const { return owned_; }

#ifdef _WIN32
        /// 本次 CoInitializeEx 的原始返回值（S_OK / S_FALSE / RPC_E_CHANGED_MODE / ...）
        HRESULT hr() const { return hr_; }
#endif

    private:
        bool owned_ = false;
#ifdef _WIN32
        HRESULT hr_ = S_OK;
#endif
    };

} // namespace cleat::Platform
