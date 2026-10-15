/**
 * @file Version.hpp
 * @brief Agent 语义化版本与统一构建标识(单一数据源)
 *
 * - kVersion: 发布版本号, 发版时手工递增(与协议版本/配置块版本互相独立)。
 * - BuildId(): 编译期构建标识。__DATE__/__TIME__ 宏是**逐编译单元展开**的 ——
 *   各 TU 直接引用会在同一构建内得到相差数秒的不同值; FirstConnect 的
 *   会话接管三元校验与 AgentDescribe 的自述必须拿到**同一个字符串**,
 *   故全仓只允许 Version.cpp 一处求值。
 */

#pragma once

namespace cleat {

    namespace ver {

        /// 语义化版本(手工维护; AgentDescribe 自述对外报这个)
        inline constexpr const char* kVersion = "0.1.0";

        /// 统一构建标识(整个构建内一致; 求值收敛在 Version.cpp, 见文件头)
        const char* BuildId();

    } // namespace ver

} // namespace cleat
