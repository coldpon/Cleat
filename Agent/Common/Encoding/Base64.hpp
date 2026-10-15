#pragma once
// Base64.hpp — 标准 base64 编解码（RFC 4648, 字母表 A-Za-z0-9+/）。
//
// 严格度: 不做任何静默修正。
//   * 字母表外字符（含空白/换行/制表）= 失败
//   * 长度必须 4 的倍数；padding 必须严格（'=' 只在末尾, 数量与尾部对应）
//   * 尾部未使用位必须为 0
//   * 任一不符 → B64Decode 返回 false, out 清空
// 调用方若需容忍 MIME 换行或 JWT 省略 padding, **自行预处理输入再传入** ——
// 本层不猜意图。历史两份实现的差异（ThirdParty/base64.hpp 静默过滤非法字符、
// 旧版容忍空白 + 省略 padding）已收敛至本文件; ThirdParty/base64.hpp 已删除。
//
// b64url（- _ 字母表）是不同字母表, 不能与本实现互换
// （DeadDropCrypto 的 b64url 走 libsodium, 与本文件无关）。

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cleat {

    // ---------- 编码 ----------
    inline std::string B64Encode(const uint8_t* data, size_t len) {
        static constexpr char kTab[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve(((len + 2) / 3) * 4);
        size_t i = 0;
        for (; i + 3 <= len; i += 3) {
            const uint32_t v = (uint32_t)data[i] << 16 | (uint32_t)data[i + 1] << 8 | data[i + 2];
            out += kTab[(v >> 18) & 63];
            out += kTab[(v >> 12) & 63];
            out += kTab[(v >> 6) & 63];
            out += kTab[v & 63];
        }
        if (len - i == 1) {
            const uint32_t v = (uint32_t)data[i] << 16;
            out += kTab[(v >> 18) & 63];
            out += kTab[(v >> 12) & 63];
            out += "==";
        } else if (len - i == 2) {
            const uint32_t v = (uint32_t)data[i] << 16 | (uint32_t)data[i + 1] << 8;
            out += kTab[(v >> 18) & 63];
            out += kTab[(v >> 12) & 63];
            out += kTab[(v >> 6) & 63];
            out += '=';
        }
        return out;
    }

    inline std::string B64Encode(std::string_view s) {
        return B64Encode(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    }

    // ---------- 解码（严格） ----------
    // 输入必须恰为合法标准 base64（含正确 padding）。失败 = out 清空 + false。
    inline bool B64Decode(std::string_view in, std::vector<uint8_t>& out) {
        static constexpr auto kDecode = []() constexpr {
            std::array<int, 256> a{};
            for (int i = 0; i < 256; ++i) a[i] = -1;
            const char tab[] =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            for (int i = 0; i < 64; ++i)
                a[static_cast<unsigned char>(tab[i])] = i;
            return a;
        }();

        out.clear();
        if (in.empty()) return true;               // 空输入 = 合法空数据
        if (in.size() % 4 != 0) return false;      // 长度非 4 倍数（含省略 padding）→ 拒

        // 尾部连续 '=' 计数（最多 2）
        size_t pad = 0;
        if (in[in.size() - 1] == '=') {
            ++pad;
            if (in[in.size() - 2] == '=') ++pad;
        }
        const size_t nvalid = in.size() - pad;

        out.reserve((in.size() / 4) * 3);

        uint32_t n = 0;
        for (size_t i = 0; i < nvalid; ++i) {
            const int d = kDecode[static_cast<unsigned char>(in[i])];
            if (d < 0) { out.clear(); return false; }   // 字母表外（含空白、中间 '='）→ 拒
            n = (n << 6) | static_cast<uint32_t>(d);
            if ((i & 3u) == 3u) {
                out.push_back(static_cast<uint8_t>((n >> 16) & 0xFF));
                out.push_back(static_cast<uint8_t>((n >> 8) & 0xFF));
                out.push_back(static_cast<uint8_t>(n & 0xFF));
                n = 0;
            }
        }

        // 尾组处理 + padding 一致性
        const size_t rem = nvalid & 3u;
        if (rem == 0) {
            if (pad != 0) { out.clear(); return false; }
        } else if (rem == 2) {
            if (pad != 2) { out.clear(); return false; }
            if (n & 0x0Fu) { out.clear(); return false; }  // 尾部 4 位必须 0
            out.push_back(static_cast<uint8_t>((n >> 4) & 0xFF));
        } else if (rem == 3) {
            if (pad != 1) { out.clear(); return false; }
            if (n & 0x03u) { out.clear(); return false; }  // 尾部 2 位必须 0
            out.push_back(static_cast<uint8_t>((n >> 10) & 0xFF));
            out.push_back(static_cast<uint8_t>((n >> 2) & 0xFF));
        } else {
            out.clear(); return false;   // rem == 1 永远非法
        }
        return true;
    }

    // 便利重载: 直接给 std::string（二进制安全, 可含 \0）
    inline bool B64Decode(std::string_view in, std::string& out) {
        std::vector<uint8_t> tmp;
        if (!B64Decode(in, tmp)) { out.clear(); return false; }
        out.assign(tmp.begin(), tmp.end());
        return true;
    }

} // namespace cleat
