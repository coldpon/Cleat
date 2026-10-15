/**
 * @file PlatformWin_Loader.cpp
 * @brief Windows PE 反射加载器实现（阶段 A）
 * @details
 * 将原始 DLL 字节反射加载到当前进程，核心流程：
 * PE 校验 → VirtualAlloc(SizeOfImage) → 复制头和节区 → 设置节区权限 →
 * 重定位（Δ = 实际基址 - ImageBase）→ IAT 绑定 → TLS 回调 → DllMain(ATTACH) → 导出枚举。
 * 反射映像不在进程模块表中，因此导出查找需自行遍历 IMAGE_EXPORT_DIRECTORY（GetProcAddress 不可用）。
 * 参考架构：pe_to_shellcode loader_v2 / sRDI Loader.cpp，自写实现。
 */

#include "PlatformWin_Loader.hpp"
#include "Common/Logger/Logger.hpp"
#include "Common/Protocol/ModuleApi.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <cstring>

namespace cleat {

    namespace {

        /**
         * @brief 将节区特性（IMAGE_SECTION_HEADER.Characteristics）转换为内存保护属性
         * @param characteristics 节区标志
         * @return 对应的 Windows 内存保护常量（PAGE_*）
         */
        DWORD SectionProtection(DWORD characteristics) {
            const bool executable = (characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
            const bool writable = (characteristics & IMAGE_SCN_MEM_WRITE) != 0;
            const bool readable = (characteristics & IMAGE_SCN_MEM_READ) != 0;
            if (executable) {
                if (writable) return PAGE_EXECUTE_READWRITE;
                if (readable) return PAGE_EXECUTE_READ;
                return PAGE_EXECUTE;
            }
            if (writable) return PAGE_READWRITE;
            if (readable) return PAGE_READONLY;
            return PAGE_NOACCESS;
        }

        /**
         * @brief 自遍历导出表（反射映像不在模块表中，GetProcAddress 不可用）
         * @param image 映像基址
         * @param opt 可选头（包含导出目录信息）
         * @return 导出名 → RVA 的键值对列表
         * @details
         * B1-2：所有 RVA 访问均经过边界校验，防止畸形 PE 导致越界读取。
         */
        std::vector<std::pair<std::string, uintptr_t>> EnumerateExports(
            void* image, const IMAGE_OPTIONAL_HEADER& opt) {
            std::vector<std::pair<std::string, uintptr_t>> out;
            const auto& dir = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
            if (!dir.VirtualAddress || !dir.Size) return out;
            const auto inImage = [&](size_t rva, size_t size) -> bool {
                return rva < opt.SizeOfImage && size <= opt.SizeOfImage - rva;
                };
            if (!inImage(dir.VirtualAddress, dir.Size)) return out;
            const auto* exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(
                static_cast<const uint8_t*>(image) + dir.VirtualAddress);
            if (!exp->NumberOfNames || !exp->NumberOfFunctions) return out;
            // 三张表（名字/序号/函数）都必须在映像内
            if (!inImage(exp->AddressOfNames,
                static_cast<size_t>(exp->NumberOfNames) * sizeof(uint32_t)) ||
                !inImage(exp->AddressOfNameOrdinals,
                    static_cast<size_t>(exp->NumberOfNames) * sizeof(uint16_t)) ||
                !inImage(exp->AddressOfFunctions,
                    static_cast<size_t>(exp->NumberOfFunctions) * sizeof(uint32_t))) {
                return out;
            }
            const auto* names = reinterpret_cast<const uint32_t*>(
                static_cast<const uint8_t*>(image) + exp->AddressOfNames);
            const auto* ordinals = reinterpret_cast<const uint16_t*>(
                static_cast<const uint8_t*>(image) + exp->AddressOfNameOrdinals);
            const auto* funcs = reinterpret_cast<const uint32_t*>(
                static_cast<const uint8_t*>(image) + exp->AddressOfFunctions);
            for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
                // 名字字符串 RVA 校验（至少 1 字节在映像内；读取限 255 字符防止越界）
                if (!inImage(names[i], 1)) continue;
                const char* name = reinterpret_cast<const char*>(
                    static_cast<const uint8_t*>(image) + names[i]);
                if (!*name) continue;
                const uint16_t ord = ordinals[i];
                if (ord >= exp->NumberOfFunctions) continue;
                if (!inImage(funcs[ord], sizeof(uint32_t))) continue;
                // 低危修复: 名字字符串可能跨出映像边界 —— 原实现只
                // 校验首字节在映像内, 随后最多读 255 字节可能越界读。读取上限取
                // min(255, 映像剩余长度), 杜绝越界。
                const size_t maxNameLen = std::min<size_t>(255,
                    static_cast<size_t>(opt.SizeOfImage) - names[i]);
                std::string s;
                for (size_t c = 0; c < maxNameLen && name[c] != '\0'; ++c) {
                    s.push_back(name[c]);
                }
                if (!s.empty()) {
                    out.emplace_back(std::move(s),
                        static_cast<uintptr_t>(funcs[ord]));
                }
            }
            return out;
        }

    } // namespace

    /**
     * @brief 反射加载 PE/DLL 模块
     * @param blob 原始 DLL 字节
     * @param info 模块信息（含 module_id）
     * @param err  错误信息输出
     * @return 加载成功的模块指针，失败返回 nullptr
     */
    std::unique_ptr<LoadedModule> PlatformWinLoader::Load(const std::vector<uint8_t>& blob,
        const ModuleInfo& info,
        std::string& err) {
        if (blob.size() < sizeof(IMAGE_DOS_HEADER)) {
            err = "blob too small";
            return nullptr;
        }
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(blob.data());
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
            err = "invalid MZ signature";
            return nullptr;
        }
        if (dos->e_lfanew <= 0 ||
            static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS) > blob.size()) {
            err = "invalid e_lfanew";
            return nullptr;
        }
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(blob.data() + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) {
            err = "invalid PE signature";
            return nullptr;
        }
        if (!(nt->FileHeader.Characteristics & IMAGE_FILE_DLL)) {
            err = "not a DLL";
            return nullptr;
        }
        const auto& opt = nt->OptionalHeader;

        //     会越界写(VirtualAlloc 只给了 SizeOfImage)。
        if (opt.SizeOfHeaders == 0 || opt.SizeOfHeaders > opt.SizeOfImage) {
            err = "invalid SizeOfHeaders (> SizeOfImage)";
            return nullptr;
        }
        //     之后读(越界读)。校验: 头部 + 全部节表项必须在 blob 内。
        const size_t headerAndSections =
            static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS)
            + static_cast<size_t>(nt->FileHeader.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER);
        if (headerAndSections > blob.size()) {
            err = "section table out of bounds";
            return nullptr;
        }
        // 位数不匹配拒绝( 升级: 原硬编码 PE64 —— x86 构建的 agent 应能
        // 加载 PE32 模块; 本文件的平台结构体(IMAGE_NT_HEADERS 等)按编译目标自动
        // 取 32/64 位变体, 判据用 SDK 的 IMAGE_NT_OPTIONAL_HDR_MAGIC = 本位数。
        // 通用位数工具在 Core/Native/Win/Arch.hpp(BOF 加载/远程注入共用)。
        if (opt.Magic != IMAGE_NT_OPTIONAL_HDR_MAGIC) {
            err = "module arch does not match host (PE32 vs PE32+)";
            return nullptr;
        }

        // 1. 分配映像内存（先 RW，稍后按节区权限修正）
        void* image = VirtualAlloc(nullptr, opt.SizeOfImage,
            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!image) {
            err = "VirtualAlloc failed";
            return nullptr;
        }

        // 2. 拷贝 PE 头（含节表）。SizeOfHeaders <= SizeOfImage(已校验)且头部
        //    在 blob 内(headerAndSections 校验含节表; SizeOfHeaders 若大于 blob
        //    实际长度则截断到 blob.size(), 节表本身已确认完整)。
        const size_t headerSize = (opt.SizeOfHeaders < blob.size()) ? opt.SizeOfHeaders : blob.size();
        std::memcpy(image, blob.data(), headerSize);

        // 3. 逐节拷贝数据
        const auto* sec = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            const size_t rva = sec[i].VirtualAddress;
            const size_t rawSize = sec[i].SizeOfRawData;
            if (rawSize > 0) {
                const size_t fileOff = sec[i].PointerToRawData;
                if (fileOff + rawSize > blob.size() || rva + rawSize > opt.SizeOfImage) {
                    err = "section out of bounds";
                    VirtualFree(image, 0, MEM_RELEASE);
                    return nullptr;
                }
                std::memcpy(static_cast<uint8_t*>(image) + rva, blob.data() + fileOff, rawSize);
            }
        }

        // B1-2：畸形 PE 防护 —— 所有目录入口/指针链 RVA 必须先过边界校验，
        // 防止损坏/恶意模块 blob 导致越界读写（映像外访问 = agent 进程内存损坏）
        const auto rvaOk = [&](size_t rva, size_t size) -> bool {
            return rva < opt.SizeOfImage && size <= opt.SizeOfImage - rva;
            };

        // 4. 重定位
        const ULONG_PTR delta = reinterpret_cast<ULONG_PTR>(image) - opt.ImageBase;
        if (delta != 0) {
            const auto& relocDir = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
            if (relocDir.VirtualAddress && relocDir.Size &&
                rvaOk(relocDir.VirtualAddress, relocDir.Size)) {
                auto* reloc = reinterpret_cast<IMAGE_BASE_RELOCATION*>(
                    static_cast<uint8_t*>(image) + relocDir.VirtualAddress);
                const uint8_t* relocEnd =
                    reinterpret_cast<const uint8_t*>(reloc) + relocDir.Size;
                while (reinterpret_cast<const uint8_t*>(reloc) + sizeof(IMAGE_BASE_RELOCATION) <= relocEnd
                    && reloc->VirtualAddress != 0 && reloc->SizeOfBlock != 0
                    && reloc->SizeOfBlock >= sizeof(IMAGE_BASE_RELOCATION)
                    && (reloc->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) % sizeof(uint16_t) == 0
                    && reinterpret_cast<const uint8_t*>(reloc) + reloc->SizeOfBlock <= relocEnd) {
                    const size_t count =
                        (reloc->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(uint16_t);
                    const auto* entries =
                        reinterpret_cast<const uint16_t*>(reinterpret_cast<const uint8_t*>(reloc) + sizeof(IMAGE_BASE_RELOCATION));
                    // 页基址必须落在映像内
                    if (!rvaOk(reloc->VirtualAddress, 0)) break;
                    uint8_t* pageBase = static_cast<uint8_t*>(image) + reloc->VirtualAddress;
                    for (size_t i = 0; i < count; ++i) {
                        const uint16_t type = entries[i] >> 12;
                        const size_t offset = entries[i] & 0x0FFF;
                        if (!rvaOk(reloc->VirtualAddress + offset, 8)) continue;  // 越界项跳过
                        if (type == IMAGE_REL_BASED_DIR64) {
                            *reinterpret_cast<uint64_t*>(pageBase + offset) += delta;
                        }
                        else if (type == IMAGE_REL_BASED_HIGHLOW) {
                            *reinterpret_cast<uint32_t*>(pageBase + offset) +=
                                static_cast<uint32_t>(delta);
                        }
                        // ABSOLUTE(0) 与其它类型跳过
                    }
                    reloc = reinterpret_cast<IMAGE_BASE_RELOCATION*>(
                        reinterpret_cast<uint8_t*>(reloc) + reloc->SizeOfBlock);
                }
            }
        }

        // 5. IAT 绑定（导入地址表解析）
        const auto& importDir = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (importDir.VirtualAddress && importDir.Size &&
            rvaOk(importDir.VirtualAddress, importDir.Size)) {
            auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
                static_cast<uint8_t*>(image) + importDir.VirtualAddress);
            const uint8_t* descEnd = reinterpret_cast<const uint8_t*>(desc) + importDir.Size;
            while (reinterpret_cast<const uint8_t*>(desc) + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= descEnd
                && desc->Name != 0) {
                if (!rvaOk(desc->Name, 1) ||
                    !rvaOk(desc->FirstThunk, sizeof(IMAGE_THUNK_DATA))) {
                    err = "invalid import descriptor";
                    VirtualFree(image, 0, MEM_RELEASE);
                    return nullptr;
                }
                const char* libName = reinterpret_cast<const char*>(
                    static_cast<uint8_t*>(image) + desc->Name);
                // 低危修复: LoadLibraryA 按默认搜索顺序(应用目录优先)
                // 加载 —— 模块 blob 可声明任意 libName, 恶意/被替换模块可借此做
                // DLL planting(在当前目录放同名 DLL)。模块契约依赖均为系统库,
                // 固定从 System32 加载(fail-closed: 非系统库依赖直接拒绝)。
                HMODULE lib = LoadLibraryExA(libName, nullptr,
                    LOAD_LIBRARY_SEARCH_SYSTEM32);
                if (!lib) {
                    err = std::string("LoadLibraryExA(System32) failed: ") + libName;
                    VirtualFree(image, 0, MEM_RELEASE);
                    return nullptr;
                }
                auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(
                    static_cast<uint8_t*>(image) + desc->FirstThunk);
                while (reinterpret_cast<const uint8_t*>(thunk) + sizeof(IMAGE_THUNK_DATA)
                    <= static_cast<const uint8_t*>(image) + opt.SizeOfImage
                    && thunk->u1.AddressOfData != 0) {
                    FARPROC proc = nullptr;
                    if (IMAGE_SNAP_BY_ORDINAL(thunk->u1.Ordinal)) {
                        proc = GetProcAddress(lib, reinterpret_cast<LPCSTR>(
                            IMAGE_ORDINAL(thunk->u1.Ordinal)));
                    }
                    else {
                        if (!rvaOk(thunk->u1.AddressOfData, sizeof(IMAGE_IMPORT_BY_NAME))) {
                            err = "invalid import by-name";
                            VirtualFree(image, 0, MEM_RELEASE);
                            return nullptr;
                        }
                        const auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                            static_cast<uint8_t*>(image) + thunk->u1.AddressOfData);
                        proc = GetProcAddress(lib, byName->Name);
                    }
                    if (!proc) {
                        err = std::string("GetProcAddress failed: ") + libName;
                        VirtualFree(image, 0, MEM_RELEASE);
                        return nullptr;
                    }
                    thunk->u1.Function = reinterpret_cast<ULONGLONG>(proc);
                    ++thunk;
                }
                ++desc;
            }
        }

        // 6. TLS: 反射加载不为模块分配线程本地存储(TLS 数据槽/索引) ——
        //    含 TLS 目录的模块(魔法静态/__declspec(thread) 依赖线程本地变量
        //    __Init_thread_epoch 等)在线程本地访问上会野读, 必须 /Zc:threadSafeInit-
        //    重编(消除 TLS 依赖)后再交付。fail-closed 并给出可执行的修复指引。
        //    (历史 bug: 旧实现 RVA 减 opt.ImageBase 而 AddressOfCallBacks 已被
        //    重定位到实际基址, delta≠0 时所有带 TLS 目录的 /MT 模块一律被拒,
        //    且报错文案 "invalid TLS callbacks" 掩盖真实原因)
        const auto& tlsDir = opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
        if (tlsDir.VirtualAddress && tlsDir.Size && rvaOk(tlsDir.VirtualAddress, tlsDir.Size)) {
            err = "module requires thread-local storage — rebuild with /Zc:threadSafeInit-";
            VirtualFree(image, 0, MEM_RELEASE);
            return nullptr;
        }

        // 6.5 统一设置节区权限（先写后锁：重定位/IAT 已改完映像，现在按节区特性锁定）
        // AD3(修复 ): 节区范围必须夹在映像内 —— 拷贝循环只在 rawSize>0 时校验
        // rva+rawSize, 而 VirtualSize > SizeOfRawData(或 SizeOfRawData==0)的畸形节区
        // 会让 VirtualProtect 作用到映像**之外**的页(改掉相邻分配的页属性 = 后续访问
        // 即崩, 或把只读数据改成可写)。
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            const size_t secRva = sec[i].VirtualAddress;
            if (secRva >= opt.SizeOfImage) continue;   // 畸形节区: 不在映像内, 跳过
            size_t virtSize = sec[i].Misc.VirtualSize
                ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData;
            if (virtSize > opt.SizeOfImage - secRva) {
                virtSize = opt.SizeOfImage - secRva;   // 夹到映像尾部
            }
            if (virtSize == 0) continue;
            DWORD oldProt = 0;
            VirtualProtect(static_cast<uint8_t*>(image) + secRva,
                virtSize, SectionProtection(sec[i].Characteristics), &oldProt);
        }

        // 7. 调用 DllMain(DLL_PROCESS_ATTACH)
        // AD3(修复 ): 入口点此前不过边界校验 —— 畸形 blob 的 AddressOfEntryPoint
        // 落在映像外时下面直接调用野地址。同文件的重定位/导入/导出都已过 rvaOk, 这里
        const DWORD epRva = opt.AddressOfEntryPoint;
        if (epRva != 0) {
            if (!rvaOk(epRva, 1)) {
                err = "entry point out of image";
                VirtualFree(image, 0, MEM_RELEASE);
                return nullptr;
            }
            const auto dllMain = reinterpret_cast<BOOL(WINAPI*)(HINSTANCE, DWORD, LPVOID)>(
                static_cast<uint8_t*>(image) + epRva);
            if (!dllMain(static_cast<HINSTANCE>(image), DLL_PROCESS_ATTACH, nullptr)) {
                err = "DllMain(DLL_PROCESS_ATTACH) returned FALSE";
                VirtualFree(image, 0, MEM_RELEASE);
                return nullptr;
            }
        }

        // 8. 导出枚举（用于命令注册）
        auto mod = std::make_unique<LoadedModule>();
        mod->base = image;
        mod->module_id = info.module_id;
        mod->export_rvas = EnumerateExports(image, opt);
        for (const auto& [name, rva] : mod->export_rvas) {
            mod->exports.push_back(name);
            (void)rva;
        }

        LOG_INFO("Loader: 反射加载成功 id=" << info.module_id
            << " base=" << image << " exports=" << mod->exports.size());
        return mod;
    }

    /**
     * @brief 卸载已加载的模块
     * @param mod 模块指针
     * @return 成功返回 true，失败返回 false
     * @details 调用 DllMain(DETACH) 后释放映像内存。
     */
    bool PlatformWinLoader::Unload(LoadedModule* mod) {
        if (!mod || !mod->base) return false;
        SIZE_T imgSize = 0;
        const auto* dos = static_cast<const IMAGE_DOS_HEADER*>(mod->base);
        if (dos->e_magic == IMAGE_DOS_SIGNATURE && dos->e_lfanew > 0) {
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
                static_cast<const uint8_t*>(mod->base) + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE) {
                imgSize = nt->OptionalHeader.SizeOfImage;
                // 模块协议允许模块自建线程(采集/钩子/reader), 引用计数只保护在途
                // handler, 不覆盖模块线程: 直接 DETACH + memset + VirtualFree 会让
                // 仍在模块代码上执行的线程踩到已清零/已释放页。
                // 契约: 模块导出可选 ModDeinit, 必须在自有线程全部退出后才返回。
                for (const auto& [name, rva] : mod->export_rvas) {
                    if (name != "ModDeinit") continue;
                    const auto deinit = reinterpret_cast<CleatModDeinitFn>(
                        static_cast<uint8_t*>(mod->base) + rva);
                    const int rc = deinit();
                    if (rc != 0) {
                        LOG_WARN("Loader: ModDeinit 返回 " << rc
                            << " (模块线程可能未完全停止), id=" << mod->module_id);
                    }
                    break;
                }
                const DWORD epRva = nt->OptionalHeader.AddressOfEntryPoint;
                if (epRva != 0) {
                    const auto dllMain = reinterpret_cast<BOOL(WINAPI*)(HINSTANCE, DWORD, LPVOID)>(
                        static_cast<uint8_t*>(mod->base) + epRva);
                    dllMain(static_cast<HINSTANCE>(mod->base), DLL_PROCESS_DETACH, nullptr);
                }
            }
        }
        // Phase D: 释放前擦除整个映像 —— 节区仍是 RX/R 等保护态, 先整体还原为 RW
        // 再清零(freed 之前的转储窗口不再有可用的模块代码/数据)
        if (imgSize != 0) {
            DWORD oldProtect = 0;
            if (VirtualProtect(mod->base, imgSize, PAGE_READWRITE, &oldProtect)) {
                memset(mod->base, 0, imgSize);
            }
        }
        VirtualFree(mod->base, 0, MEM_RELEASE);
        mod->base = nullptr;
        LOG_INFO("Loader: 模块已卸载 id=" << mod->module_id);
        return true;
    }

} // namespace cleat