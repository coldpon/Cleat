/**
 * @file CommsManager.cpp
 * @brief 通信管理器实现
 */

#include "CommsManager.hpp"
#include "Core/Comms/Channel/Chain.hpp"
#include "Common/Logger/Logger.hpp"

#include <utility>

namespace cleat {

    /**
     * @brief 构造函数
     * @param chain 通道链引用
     */
    CommsManager::CommsManager(Chain& chain)
        : chain_(chain) {
    }

    /**
     * @brief 析构函数，自动断开连接
     */
    CommsManager::~CommsManager() {
        Disconnect();
    }

    /**
     * @brief 建立连接（委托给 Chain）
     * @param host 目标主机
     * @param port 目标端口
     * @return 连接成功返回 true，否则 false
     */
    bool CommsManager::Connect(const std::string& host, uint16_t port) {
        return chain_.Connect(host, port);
    }

    /**
     * @brief 断开连接（委托给 Chain）
     */
    void CommsManager::Disconnect() {
        chain_.Disconnect();
    }

    /**
     * @brief 整体替换候选链(热切换, 委托 Chain)
     */
    void CommsManager::ReplaceChannels(ChannelCandidates cands) {
        chain_.SetCandidates(std::move(cands));
    }

    // 说明: Chain::Send 返回 SendResult(的五值)。本层仍以 bool 向调用方
    // 表达"本帧是否已被消费" —— 该收口将在 (OutboundSink)一并改造。
    // 映射: Queued/Sent/Dropped → 已消费(true); Rejected/ChannelFailure → 未消费(false,
    // 帧回插队首待重试, 与旧实现一致)。


    /**
     * @brief 检查是否已连接（委托给 Chain）
     * @return true 已连接，false 未连接
     */
    bool CommsManager::IsConnected() const {
        return chain_.IsConnected();
    }

    /**
     * @brief 检查当前通道是否为低带宽（委托给 Chain）
     * @return true 低带宽（DNS/ICMP），false 高带宽
     */
    bool CommsManager::IsLowBandwidth() const {
        return chain_.IsLowBandwidth();
    }

    uint32_t CommsManager::NextPollDueInMs() const {
        return chain_.NextPollDueInMs();
    }

    uint32_t CommsManager::PendingUplinkBytes() const {
        return chain_.PendingUplinkBytes();
    }

    /**
     * @brief 取一帧（N32/L-1: 唯一收包入口）
     * @details 直通 `Chain::Recv` —— 回探与收侧失败计数都在那里, 生产与夹具因此同路。
     *          旧的两条入站路径(`SetOnPacketCallback`+`ProcessIncoming` 与
     *          `Chain::Recv` 直取)已合并为这一条。
     */
    bool CommsManager::RecvFrame(std::vector<uint8_t>& frame) {
        return chain_.Recv(frame, 0);
    }

} // namespace cleat