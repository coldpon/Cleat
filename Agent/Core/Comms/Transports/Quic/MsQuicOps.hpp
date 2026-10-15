/**
 * @file MsQuicOps.hpp
 * @brief IQuicOps 的 msquic 实现(详细状态 PIMPL 封在 cpp)。
 * @details msquic.h 只出现在 .cpp 中 —— 通道层头文件不引入第三方类型。
 *          连接模型: 单条双向 stream 承载会话帧字节流; 收包经 msquic 回调投递到
 *          内部队列, RecvStreamData 消费(与 WsTransport 的收件箱同构)。
 *          发送缓冲生命周期覆盖至 SEND_COMPLETE(msquic 异步读取 —— 实测踩坑:
 *          栈上 QUIC_BUFFER 会发出被复用的栈内存, 长度对而内容为垃圾)。
 */
#pragma once

#include <memory>

#include "Core/Comms/Transports/Quic/IQuicOps.hpp"

namespace cleat {

    class MsQuicOps : public IQuicOps {
    public:
        MsQuicOps();
        ~MsQuicOps() override;

        MsQuicOps(const MsQuicOps&) = delete;
        MsQuicOps& operator=(const MsQuicOps&) = delete;
        MsQuicOps(MsQuicOps&&) = delete;
        MsQuicOps& operator=(MsQuicOps&&) = delete;

        bool Handshake(const TransportConfig& cfg) override;
        bool SendStreamData(const uint8_t* data, size_t len) override;
        int RecvStreamData(std::vector<uint8_t>& out, int timeout_ms) override;
        void Close() override;
        bool IsAlive() const override;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    /**
     * @brief 进程级 QUIC 污染标记(睡眠掩码, )
     * @return true = MsQuicOpen2 在本进程成功执行过(此后掩码窗口永久禁用);
     *         msquic 未编入(CLEAT_NO_QUIC / 非 Windows)时恒 false。
     * @details v1 零风险策略: msquic 平台线程池(逻辑核数条, start 地址静态链入本镜像)
     *          在"关闭后是否真退出"实测确认前, 双保险直接拒绝掩码。
     */
    bool QuicEverOpened();

} // namespace cleat
