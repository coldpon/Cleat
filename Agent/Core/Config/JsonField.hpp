/**
 * @file JsonField.hpp
 * @brief 配置 JSON 字段读取工具：类型安全读取 + 数值钳位（全 inline）
 * @details
 * 从 AgentConfig.cpp 拆出的通用工具层，供配置序列化（AgentConfigJson.cpp）
 * 及未来其他配置模块复用：
 * - ReadUInt / ReadInt：整数读取，支持可选范围校验（超出范围视为无效配置）
 * - ReadString / ReadBool：字符串 / 布尔读取
 * - Clamp：数值钳位（模板，type_identity 抑制实参推导，强制三参同型）
 * 本头文件只依赖标准库与 json.hpp，不依赖任何配置结构体。
 * 全部符号置于 cleat::detail，避免污染 cleat 命名空间。
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <type_traits>

#include "Common/ThirdParty/json.hpp"

namespace cleat::detail {

    // ---- type_identity：抑制模板实参推导，让 lo/hi 强制与 v 同型 ----
    template <typename T> struct type_identity { using type = T; };
    template <typename T> using type_identity_t = typename type_identity<T>::type;

    /**
     * @brief 从 JSON 中读取无符号整数，支持范围校验（可选）
     * @tparam T 整数类型（无符号）
     * @param src  JSON 对象
     * @param key  要读取的键
     * @param out  输出变量（仅在成功时赋值）
     * @param min  允许的最小值（默认类型最小值）
     * @param max  允许的最大值（默认类型最大值）
     * @return 成功读取且值在有效范围内返回 true，否则返回 false
     * @note (第十七轮): 只接受**整型** —— 旧写法 `is_number()` 对 float 亦为真,
     *       越界 double→整型 `static_cast` 是 UB(截断值超出目标可表示范围), 且钳位
     *       发生在转换之后救不回来。负的 number_integer 也按无效拒绝(负值经
     *       `get<uint64_t>()` 会回绕成巨数, 属"碰巧被范围检查兜住"的侥幸)。
     */
    template <typename T>
    bool ReadUInt(const nlohmann::json& src, const char* key, T& out,
        type_identity_t<T> min = std::numeric_limits<T>::min(),
        type_identity_t<T> max = std::numeric_limits<T>::max()) {
        static_assert(std::is_unsigned<T>::value, "ReadUInt requires an unsigned type");
        auto it = src.find(key);
        if (it == src.end()) return false;
        if (it->is_number_unsigned()) {
            const uint64_t raw = it->get<uint64_t>();       // 原生无符号: 精确
            if (raw < static_cast<uint64_t>(min) || raw > static_cast<uint64_t>(max))
                return false;
            out = static_cast<T>(raw);
            return true;
        }
        if (it->is_number_integer()) {
            const int64_t raw = it->get<int64_t>();         // 原生有符号: 先验符号再收窄
            if (raw < 0 || raw < static_cast<int64_t>(min) || raw > static_cast<int64_t>(max))
                return false;
            out = static_cast<T>(raw);
            return true;
        }
        return false;   // float / string / bool / null 一律无效()
    }

    /**
     * @brief 从 JSON 中读取有符号整数，支持范围校验（可选）
     * @tparam T 整数类型（有符号）
     * @param src  JSON 对象
     * @param key  要读取的键
     * @param out  输出变量
     * @param min  允许的最小值
     * @param max  允许的最大值
     * @return 成功读取且值在有效范围内返回 true，否则返回 false
     * @note (第十七轮): 只接受**整型**(拒绝 float, 理由同 ReadUInt)。number_unsigned
     *       分支必须**先在无符号域比较**再收窄 —— 直接 `get<int64_t>()` 对 > INT64_MAX
     *       的值是实现定义的回绕, 回绕后可能"碰巧"落进 [min, max] 被接受。
     */
    template <typename T>
    bool ReadInt(const nlohmann::json& src, const char* key, T& out,
        type_identity_t<T> min = std::numeric_limits<T>::min(),
        type_identity_t<T> max = std::numeric_limits<T>::max()) {
        static_assert(std::is_signed<T>::value, "ReadInt requires a signed type");
        auto it = src.find(key);
        if (it == src.end()) return false;
        if (it->is_number_unsigned()) {
            if (max < 0) return false;                      // 无符号值不可能 ≤ 负上限
            const uint64_t raw = it->get<uint64_t>();
            if (min >= 0 && raw < static_cast<uint64_t>(min)) return false;
            if (raw > static_cast<uint64_t>(max)) return false;
            out = static_cast<T>(raw);                      // 已验证可表示, 收窄安全
            return true;
        }
        if (it->is_number_integer()) {
            const int64_t raw = it->get<int64_t>();
            if (raw < static_cast<int64_t>(min) || raw > static_cast<int64_t>(max))
                return false;
            out = static_cast<T>(raw);
            return true;
        }
        return false;   // float / string / bool / null 一律无效()
    }

    /**
     * @brief 从 JSON 中读取字符串字段
     * @param src JSON 对象
     * @param key 要读取的键
     * @param out 输出字符串
     * @return 存在且为字符串类型时返回 true 并更新 out，否则返回 false
     * @note 空字符串可用于清除显式值，恢复随机默认行为
     */
    inline bool ReadString(const nlohmann::json& src, const char* key, std::string& out) {
        auto it = src.find(key);
        if (it == src.end() || !it->is_string()) return false;
        out = it->get<std::string>();
        return true;
    }

    /**
     * @brief 从 JSON 中读取布尔字段
     * @param src JSON 对象
     * @param key 要读取的键
     * @param out 输出布尔值
     * @return 存在且为布尔类型时返回 true 并更新 out，否则返回 false
     */
    inline bool ReadBool(const nlohmann::json& src, const char* key, bool& out) {
        auto it = src.find(key);
        if (it == src.end() || !it->is_boolean()) return false;
        out = it->get<bool>();
        return true;
    }

    /**
     * @brief 钳制数值到指定范围 [lo, hi]
     * @tparam T 数值类型（三参同型；type_identity 抑制后两参的实参推导，
     *           避免 int 字面量隐式转换导致的类型歧义）
     * @param v  输入值
     * @param lo 下限
     * @param hi 上限
     * @return 钳制后的值
     */
    template <typename T>
    T Clamp(T v, type_identity_t<T> lo, type_identity_t<T> hi) {
        return std::max(lo, std::min(v, hi));
    }

} // namespace cleat::detail