/**
 * @file CommandRequest.hpp
 * @brief 命令执行的完整请求对象
 * @details
 * **一次机械改动同时解决三件事**(F.10): ①`binary_data` 的接口噪音 ②`CommandContext`
 * 无论如何要加(的 epoch + 带宽决策依据 + 剩余超时 + 目标资源)③`ResourceKey` 的声明机制。
 *
 * 为什么**合并成单参数**而不是给 `Execute` 再加一个 `ctx`: 66 个 handler 今天接受 2 个参数
 * (其中 1 个从不使用), 若分两次改, 同样这些文件要动两次。合并成 `CommandRequest` 只动一次。
 *
 * 为什么**去掉默认参数**: 原形态 `Execute(params, binary_data = {})` 里的默认值让该参数
 * **看起来**可选 —— 这正是"没人用 binary_data"这一误判的来源之一(F.9 已核实有 4 个在树
 * 消费者依赖它)。新形态无默认参数, 必须显式传。
 */
#pragma once

#include <cstdint>
#include <vector>

#include "Common/ThirdParty/json.hpp"
#include "Core/Command/Handlers/ResourceKey.hpp"

namespace cleat {

    /// 出站带宽档(与 `ChannelCaps.hpp` 的 `BandwidthClass` 同义; 此处仅作别名以便阅读)
    enum class BandwidthClass;

    /**
     * @brief 命令执行的上下文
     * @details
     * **边界(重要)**: `epoch` 只隔离**可见产物**(迟到响应、陈旧确认帧), **不隔离副作用** ——
     * 被遗弃的执行线程仍会修改共享资源; 该缺口由 `resource` 对应的互斥补, 二者共同构成
     * 超时语义方案 (b)。
     */
    struct CommandContext {
        uint64_t epoch = 0;        ///<  该命令所属连接代际(0 = 未接线)
        int bandwidth = 0;         ///< E: 执行前带宽决策依据(0=High 1=Low; 见 ChannelCaps)
        uint64_t deadline_ms = 0;  ///< 剩余超时(毫秒; 0 = 不限时)
        ResourceKey resource;      ///<  目标资源(空 = 不参与互斥)
        /**
         * @brief 二进制响应体出口(可选): 截图等大载荷命令的混合帧 body。
         * @details
         * 指针由 CommandProcessor 指向其栈上槽位, handler 执行时一次性写入 ——
         * **一次执行、一份产物**: 截图只能拍一张, 若走"二次查询"式接口会重复捕获。
         * 默认 nullptr = 纯 JSON 响应(66 个既有 handler 零波及)。写入方在执行
         * 线程内, 无并发。响应出站时由 msg::Response 以混合帧形态携带
         * (OutboundMessages.hpp), 服务端 decodeResponse 拆出提升。
         */
        std::vector<uint8_t>* binary_out = nullptr;
    };

    /**
     * @brief 一次命令执行的完整输入
     */
    struct CommandRequest {
        const nlohmann::json& params;          ///< 请求参数
        const std::vector<uint8_t>& payload;   ///< 混合帧二进制体(旧名 `binary_data`; 空 = 无)
        const CommandContext& ctx;             ///< 上文(代际/带宽/剩余超时/资源)
    };

} // namespace cleat
