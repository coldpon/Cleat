/**
 * @file ApiResolver.hpp
 * @brief 敏感 API 动态解析(导入表脱敏)
 *
 * 为什么: OpenProcess/WinHttpOpen/CreateThread/VirtualProtectEx 等"攻击面 API"
 * 静态链接会出现在导入表(IAT)中, 蓝队/EDR 静态扫描一键可见。GetProcAddress/
 * LoadLibrary 本身不敏感(人人都在用), 本项目不藏它们 —— 藏的是"敏感 API 名"。
 *
 * 用法: 调用点不再静态链接敏感 API, 改为:
 *   static const auto pOpenProcess = cleat::ApiResolver::Proc<HANDLE(WINAPI*)(DWORD,BOOL,DWORD)>(
 *       OBF_STR("kernel32.dll"), OBF_STR("OpenProcess"));
 *   pOpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
 *
 * 效果: 二进制导入表无 OpenProcess 字样, 函数名仅以 OBF 密文存在于 .rdata。
 * API 名经 OBF_STR 编译期混淆(见 Common/StringObf.hpp), 结果缓存避免重复解析。
 */

#pragma once

#include "Common/Obfuscation/StringObf.hpp"
#include <windows.h>
#include <mutex>
#include <string>
#include <unordered_map>

namespace cleat {

    class ApiResolver {
    public:
        /// 解析模块句柄(内部 LoadLibraryExA 限定 System32; 模块名 OBF 混淆)
        static HMODULE Module(const std::string& moduleName) {
            std::lock_guard<std::mutex> lock(g_mu());
            auto& cache = g_modCache();
            auto it = cache.find(moduleName);
            if (it != cache.end()) {
                return it->second;
            }
            // AD1(修复 ): 此处传的是**裸名**, 裸名 LoadLibraryA 走默认搜索顺序
            // (应用所在目录优先)→ 可写目录里放个同名 DLL 即可劫持解析(经典 DLL
            // planting)。本 resolver 的模块名全是系统库(kernel32/user32/advapi32/
            // 取, 取不到返回 nullptr(调用点全部判空 = fail-closed)。
            HMODULE h = ::LoadLibraryExA(moduleName.c_str(), nullptr,
                LOAD_LIBRARY_SEARCH_SYSTEM32);
            // AD4: 只缓存成功结果 —— 负结果永久缓存会让一次瞬时失败在进程余下
            // 生命周期内不可恢复(调用点只看到永久 nullptr)。
            if (h) {
                cache[moduleName] = h;
            }
            return h;
        }

        /// 解析导出函数地址(内部 GetProcAddress; 函数名 OBF 混淆)
        static FARPROC ProcRaw(const std::string& moduleName, const std::string& procName) {
            const std::string key = moduleName + "!" + procName;
            {
                std::lock_guard<std::mutex> lock(g_mu());
                auto& cache = g_procCache();
                auto it = cache.find(key);
                if (it != cache.end()) {
                    return it->second;
                }
            }
            // 锁外取模块 + 解析: 避免与 Module() 的重入死锁(resource deadlock,
            //  实测 HTTPS 启动即崩)。复用 Module() 的模块缓存 —— 同一模块
            // 只 LoadLibraryA 一次, 不再每次缓存未命中就重复 Load(模块引用计数
            // 只增不减, 且绕过 Module() 缓存导致同模块多份句柄)。
            // Module() 内部自带锁, 此处已在锁外, 无嵌套持锁。
            HMODULE h = Module(moduleName);
            FARPROC p = h ? ::GetProcAddress(h, procName.c_str()) : nullptr;
            if (p) {
                std::lock_guard<std::mutex> lock(g_mu());
                g_procCache()[key] = p;   // AD4: 同上, 负结果不缓存(留待下次重试)
            }
            return p;
        }

        /// 类型化解析: 返回强类型函数指针(调用前判空)
        template <typename Fn>
        static Fn Proc(const std::string& moduleName, const std::string& procName) {
            return reinterpret_cast<Fn>(ProcRaw(moduleName, procName));
        }

    private:
        static std::mutex& g_mu() {
            static std::mutex m;
            return m;
        }
        static std::unordered_map<std::string, HMODULE>& g_modCache() {
            static std::unordered_map<std::string, HMODULE> c;
            return c;
        }
        static std::unordered_map<std::string, FARPROC>& g_procCache() {
            static std::unordered_map<std::string, FARPROC> c;
            return c;
        }
    };

} // namespace cleat