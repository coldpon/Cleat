/**
 * @file PlatformWin_RemoteMapper.hpp
 * @brief 载荷准备层(native_dll → 可注入 blob) —— 反射加载器入口
 * @details
 * **形态 A'**: 复用 **ShellcodeLoader** 的 boot+PIC loader
 * (仓库自有、迁移/夹具 E2E 已验证的唯一 loader 实现), 在 Agent 侧把裸 PE DLL
 * 在线打包成 `[boot][loader][PE]` 反射加载 shellcode —— 产物满足 Blob 契约
 * (第 0 字节 = 入口), 直接走既有 Blob 注入路径。loader 在目标内完成段映射/
 * 重定位/IAT 绑定/DllMain(ATTACH), 不存在第二份 loader 实现(与 dll2shc 交付
 * 给操作员的产物逐字节同源)。
 *
 * 保留为后续升级路径 —— 仅当需要 entry/args 契约(指定导出 + 参数)或输出回收
 * 时再立项; 届时管线自本文件之后的调用方无感替换。
 *
 * 校验口径(fail-closed): MZ/PE32+/x64/IMAGE_FILE_DLL/无 TLS 目录 —— TLS 与
 * 核心加载器同款拒载。
 */
#pragma once

#include "Core/Inject/InjectionTypes.hpp"

#include <string>
#include <vector>

namespace cleat {
namespace injection {

    /// 载荷准备统一入口(手法调用, 契约校验后的唯一变体点):
    ///   * Blob      → 原样透传(第 0 字节 = 入口);
    ///   * NativeDll → 打包成 [boot][loader][PE] 反射加载 shellcode
    ///                 (ShellcodeLoader 同源 stub, DllMain(ATTACH) 目标内执行)。
    /// 失败返回 false + err(非 x64 PE32+ DLL/带 TLS 目录/超限等)。
    /// 成功后 out 按 Blob 契约注入, 手法对其余流程零感知。
    bool PreparePayload(const std::vector<uint8_t>& payload, PayloadKind kind,
        std::vector<uint8_t>& out, std::string& err);

} // namespace injection
} // namespace cleat
