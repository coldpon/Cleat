/**
 * @file StringObf.hpp
 * @brief 编译期字符串混淆（静态特征摘除）
 * @details
 * 提供编译期 XOR 混淆的字符串字面量，用于防止静态分析和字符串特征匹配。
 *
 * 用法示例：
 * @code
 *   std::string s = OBF_STR("cmd.exe");              // 窄字符串
 *   std::wstring w = OBF_WSTR(L"SOFTWARE\\...");     // 宽字符串（注册表路径等）
 * @endcode
 *
 * 原理：
 * constexpr 构造时逐字节 XOR（编译期完成），明文只存在于编译期，二进制中仅包含密文。
 * 运行时调用 .str() 解出到栈或堆上。密钥由 __COUNTER__ 派生，同一字符串在不同调用点
 * 使用不同密钥 → 密文不同，防止模式匹配和频率分析。
 *
 * 局限性（如实说明）：
 * - 防静态分析 / 防字符串 grep；不防内存 dump（解密瞬间栈/堆上有明文）——
 *   内存侧防护由 Sleep Mask（休眠时加密）承担，两者互补。
 * - 每次调用 OBF_STR() 都有一次 XOR 循环开销（微秒级，冷路径无感）。
 * - 仅适用于字面量；变量请先用 std::string 再采用其他混淆手段。
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace cleat::obf {

    /**
     * @brief 窄字符串编译期混淆类
     * @tparam N   字符数组长度（包括末尾 '\\0'）
     * @tparam Key XOR 混淆密钥（8 位）
     *
     * 在编译期将输入字符串逐字符与 Key 异或，存储密文；运行时通过 str() 解密还原。
     */
    template <size_t N, uint8_t Key>
    struct ObfString {
        char data[N];   ///< 存储混淆后的密文字符数组

        /**
         * @brief 编译期构造函数
         * @param s 原始字符串字面量（以 '\\0' 结尾）
         *
         * 在编译期对 s 的每个字符（包括结尾 '\\0'）与 Key 异或，存入 data。
         */
        constexpr ObfString(const char(&s)[N]) : data{} {
            for (size_t i = 0; i < N; ++i) {
                data[i] = static_cast<char>(static_cast<uint8_t>(s[i]) ^ Key);
            }
        }

        /**
         * @brief 运行时还原原始字符串
         * @return std::string 还原后的明文字符串
         *
         * 将 data 中除末尾 '\\0' 外的每个字节与 Key 异或，得到原始内容。
         */
        std::string str() const {
            std::string out;
            out.reserve(N - 1);
            for (size_t i = 0; i + 1 < N; ++i) {
                out.push_back(static_cast<char>(static_cast<uint8_t>(data[i]) ^ Key));
            }
            return out;
        }
    };

    /**
     * @brief 宽字符串编译期混淆类
     * @tparam N   宽字符数组长度（包括末尾 L'\\0'）
     * @tparam Key XOR 混淆密钥（16 位）
     *
     * 与 ObfString 类似，但用于 wchar_t 宽字符串，密钥为 16 位。
     */
    template <size_t N, uint16_t Key>
    struct ObfWString {
        wchar_t data[N];   ///< 存储混淆后的密文宽字符数组

        /**
         * @brief 编译期构造函数
         * @param s 原始宽字符串字面量（以 L'\\0' 结尾）
         */
        constexpr ObfWString(const wchar_t(&s)[N]) : data{} {
            for (size_t i = 0; i < N; ++i) {
                data[i] = static_cast<wchar_t>(static_cast<uint16_t>(s[i]) ^ Key);
            }
        }

        /**
         * @brief 运行时还原原始宽字符串
         * @return std::wstring 还原后的明文字符串
         */
        std::wstring str() const {
            std::wstring out;
            out.reserve(N - 1);
            for (size_t i = 0; i + 1 < N; ++i) {
                out.push_back(static_cast<wchar_t>(static_cast<uint16_t>(data[i]) ^ Key));
            }
            return out;
        }
    };

} // namespace cleat::obf

/**
 * @def OBF_STR(s)
 * @brief 窄字符串混淆宏
 * @param s 窄字符串字面量（例如 "cmd.exe"）
 * @return std::string 还原后的明文字符串
 *
 * 利用 __COUNTER__ 生成每个调用点独立的密钥，强制编译期构造并返回还原后的字符串。
 * 示例：std::string cmd = OBF_STR("cmd.exe");
 */
#define OBF_STR(s) \
    ([]() { constexpr auto obf_ = ::cleat::obf::ObfString<sizeof(s), static_cast<uint8_t>(((__COUNTER__ * 131 + 7) % 255) + 1)>(s); return obf_.str(); }())

 /**
  * @def OBF_WSTR(s)
  * @brief 宽字符串混淆宏
  * @param s 宽字符串字面量（例如 L"SOFTWARE\\Microsoft"）
  * @return std::wstring 还原后的明文字符串
  *
  * 用于宽字符串，密钥为 16 位，同样每个调用点独立。
  * 注意：sizeof(s) / sizeof(wchar_t) 计算元素个数（包括 L'\\0'）。
  */
#define OBF_WSTR(s) \
    ([]() { constexpr auto obf_ = ::cleat::obf::ObfWString<sizeof(s) / sizeof(wchar_t), static_cast<uint16_t>(((__COUNTER__ * 2621 + 13) % 65535) + 1)>(s); return obf_.str(); }())

