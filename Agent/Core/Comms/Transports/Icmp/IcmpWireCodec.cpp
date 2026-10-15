/**
 * @file IcmpWireCodec.cpp
 * @brief ICMP 线格式实现
 */

#include "Core/Comms/Transports/Icmp/IcmpWireCodec.hpp"

namespace cleat {
namespace icmp {

    std::vector<uint8_t> WinPingCover() {
        std::vector<uint8_t> cover(kWinPingDataLen);
        for (size_t i = 0; i < cover.size(); ++i) {
            cover[i] = static_cast<uint8_t>(0x61 + (i % 23));   // 'a'..'w' 循环
        }
        return cover;
    }

    bool IsCoverEcho(const std::vector<uint8_t>& reply) {
        const std::vector<uint8_t> cover = WinPingCover();
        return reply == cover;
    }

} // namespace icmp
} // namespace cleat
