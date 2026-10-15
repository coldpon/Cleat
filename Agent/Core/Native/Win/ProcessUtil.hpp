/**
 * @file ProcessUtil.hpp
 * @brief 进程平台工具(判活) —— 原 Injector.hpp 的非注入导出归位(:
 *        SpawnAndInject 整合进 Core/Inject 后, 本文件只剩与注入无关的通用件)
 */
#pragma once

#ifdef _WIN32

namespace cleat {
    namespace native {

        /// 进程是否仍存活(平台层导出, 规范 1: 平台 API 不出 Platform 文件)。
        /// 判定失败(打不开句柄/权限不足)一律按"不存活"返回 —— 与调用方
        /// "无可依赖的存活证据就不自退"的保守取向一致。
        bool IsProcessAlive(unsigned long pid);

    } // namespace native
} // namespace cleat

#endif // _WIN32
