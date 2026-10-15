#include "Core/Loader/BOF/CoffLoader.hpp"

#ifdef _WIN32

#include "Core/Loader/BOF/BofApi.hpp"
#include "Core/Loader/BOF/CoffDefs.hpp"
#include "Core/Native/Win/Arch.hpp"
#include "Common/Logger/Logger.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// COFF/BOF 加载执行: 校验 → 映射 → 重定位 → 入口 → 执行 → 收尸。
// 重定位口径与 AdaptixC2 beacon/bof_loader.cpp 一致(x64 ADDR64/ADDR32NB/
// REL32_1-5, x86 DIR32/REL32, 补 SECTION/SECREL); 输出走 per-execution
// 上下文(BofApi.hpp), 架构校验收口到 Arch.hpp。支持 x86 与 x64(

namespace cleat {
    namespace coff {
        namespace {

            char HexDigit(unsigned v) {
                return "0123456789abcdef"[v & 0xF];
            }

            std::string Hex32(uint32_t v) {
                std::string s = "0x";
                for (int i = 28; i >= 0; i -= 4) {
                    s.push_back(HexDigit(v >> i));
                }
                return s;
            }

            /// 符号名: 短名(≤8 字节, 可能非 NUL 结尾)或字符串表偏移。
            /// strTabLen 以字符串表 4 字节长度前缀为准(截断到 blob 边界)。
            std::string SymbolName(const Symbol& sym, const uint8_t* strTab,
                size_t strTabLen, bool& ok) {
                ok = true;
                if (sym.Name.dwName[0] == 0) {
                    const uint32_t off = sym.Name.dwName[1];
                    if (strTabLen == 0 || off >= strTabLen) {
                        ok = false;
                        return {};
                    }
                    const char* p = reinterpret_cast<const char*>(strTab + off);
                    const size_t max = strTabLen - off;
                    const void* nul = memchr(p, 0, max);
                    return nul ? std::string(p, static_cast<size_t>(static_cast<const char*>(nul) - p))
                        : std::string(p, max);
                }
                size_t n = 0;
                while (n < 8 && sym.Name.cName[n]) {
                    ++n;
                }
                return std::string(sym.Name.cName, n);
            }

            // __imp_ 引用: mingw 对 dllimport 声明的外部函数生成 __imp_X(x64) /
            // __imp__X(x86, 另带 @N stdcall 尾)符号, 重定位落在**指针槽位**上。
            bool IsImpRef(const std::string& name) {
                // 7 字节前缀先判(否则 "__imp__x" 会被 6 字节截走一个下划线)
                return name.compare(0, 7, "__imp__") == 0 || name.compare(0, 6, "__imp_") == 0;
            }

            std::string StripImpRef(const std::string& name) {
                if (name.compare(0, 7, "__imp__") == 0) {
                    return name.substr(7);
                }
                if (name.compare(0, 6, "__imp_") == 0) {
                    return name.substr(6);
                }
                return name;
            }

            /// Beacon* API 按名查找(明文比较, 口径见 BofApi.hpp)
            void* FindBeaconApi(const std::string& name) {
                size_t count = 0;
                const bof::ApiEntry* table = bof::ApiTable(count);
                for (size_t i = 0; i < count; ++i) {
                    if (name == table[i].name) {
                        return table[i].proc;
                    }
                }
                return nullptr;
            }

            /// CS 生态约定: MODULE$FUNC(如 KERNEL32$Sleep; x86 带标准调用 @N 尾)。
            void* ResolveModuleFunc(const std::string& name) {
                const size_t sep = name.find('$');
                if (sep == std::string::npos || sep == 0 || sep + 1 >= name.size()) {
                    return nullptr;
                }
                std::string func = name.substr(sep + 1);
                const size_t at = func.find('@');
                if (at != std::string::npos && at > 0) {
                    bool allDigits = at + 1 < func.size();
                    for (size_t i = at + 1; i < func.size(); ++i) {
                        if (func[i] < '0' || func[i] > '9') {
                            allDigits = false;
                            break;
                        }
                    }
                    if (allDigits) {
                        func.resize(at);
                    }
                }
                // LOAD_LIBRARY_SEARCH_SYSTEM32 只约束裸名搜索, 显式路径(如
                // C:\\evil\\a.dll$func)会按文档直通, "固定 System32"注释因此失真。
                const std::string module = name.substr(0, sep);
                if (module.find_first_of("\\/:") != std::string::npos) {
                    return nullptr;
                }
                return ApiResolver::ProcRaw(module, func);
            }

            void* ResolveExternal(const std::string& name) {
                if (void* p = FindBeaconApi(name)) {
                    return p;
                }
                return ResolveModuleFunc(name);
            }

            /// 单个重定位的应用(按目标架构的补丁口径写回 site)。
            /// siteSize 为补丁点所在节区的映射大小, 用于写入宽度越界防护。
            bool ApplyRelocation(uint16_t machine, const Relocation& rel, uint8_t* site,
                size_t siteSize, void* target, const std::string& symName,
                std::string& err) {
                // 各类型写入宽度(ADDR64=8, SECTION=2, 其余 4)
                size_t width = 4;
                if (machine == kMachineAmd64 && rel.Type == kRelAmd64Addr64) {
                    width = 8;
                }
                else if (rel.Type == kRelAmd64Section || rel.Type == kRelI386Section) {
                    width = 2;
                }
                if (static_cast<size_t>(rel.VirtualAddress) + width > siteSize) {
                    err = "relocation site out of section bounds (section offset "
                        + Hex32(rel.VirtualAddress) + ", symbol " + symName + ")";
                    return false;
                }

                if (machine == kMachineAmd64) {
                    switch (rel.Type) {
                    case kRelAmd64Addr64: {
                        // S + A(64 位绝对)
                        uint64_t v = 0;
                        memcpy(&v, site, sizeof(v));
                        v += reinterpret_cast<uint64_t>(target);
                        memcpy(site, &v, sizeof(v));
                        return true;
                    }
                    case kRelAmd64Addr32Nb: {
                        // S + A - P, 无基址 32 位
                        int32_t a = 0;
                        memcpy(&a, site, sizeof(a));
                        const int64_t d = reinterpret_cast<int64_t>(target) + a
                            - reinterpret_cast<int64_t>(site);
                        if (d < 0 || d > 0xFFFFFFFFll) {
                            err = "ADDR32NB overflow (symbol " + symName + ")";
                            return false;
                        }
                        const uint32_t v = static_cast<uint32_t>(d);
                        memcpy(site, &v, sizeof(v));
                        return true;
                    }
                    case kRelAmd64Rel32:
                    case kRelAmd64Rel32 + 1:
                    case kRelAmd64Rel32 + 2:
                    case kRelAmd64Rel32 + 3:
                    case kRelAmd64Rel32 + 4:
                    case kRelAmd64Rel32 + 5: {
                        // S + A - P; P 的尾差由指令后续字节数决定(Type-4)
                        int32_t a = 0;
                        memcpy(&a, site, sizeof(a));
                        const size_t trail = 4 + static_cast<size_t>(rel.Type - kRelAmd64Rel32);
                        const int64_t d = reinterpret_cast<int64_t>(target) + a
                            - reinterpret_cast<int64_t>(site + trail);
                        if (d > 0x7FFFFFFFll || d < -0x80000000ll) {
                            err = "REL32 overflow (symbol " + symName + ")";
                            return false;
                        }
                        const int32_t v = static_cast<int32_t>(d);
                        memcpy(site, &v, sizeof(v));
                        return true;
                    }
                    case kRelAmd64Section: {
                        // 16 位节区号(由调用方给出, target 复用为节区号)
                        const uint16_t v = static_cast<uint16_t>(reinterpret_cast<uintptr_t>(target));
                        memcpy(site, &v, sizeof(v));
                        return true;
                    }
                    case kRelAmd64SecRel: {
                        // 32 位节区内偏移(target 复用为偏移; 值与 kRelI386SecRel 同为
                        // 0x000B, 各架构 switch 只列自家常量, 避免 duplicate case)
                        const uint32_t v = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(target));
                        memcpy(site, &v, sizeof(v));
                        return true;
                    }
                    default:
                        err = "unsupported AMD64 relocation type " + Hex32(rel.Type)
                            + " (symbol " + symName + ")";
                        return false;
                    }
                }

                // ---- I386 ----
                switch (rel.Type) {
                case kRelI386Dir32: {
                    // S + A(32 位绝对)
                    uint32_t v = 0;
                    memcpy(&v, site, sizeof(v));
                    v += static_cast<uint32_t>(reinterpret_cast<uintptr_t>(target));
                    memcpy(site, &v, sizeof(v));
                    return true;
                }
                case kRelI386Rel32: {
                    // S + A - P
                    int32_t a = 0;
                    memcpy(&a, site, sizeof(a));
                    const int64_t d = reinterpret_cast<int64_t>(target) + a
                        - reinterpret_cast<int64_t>(site + 4);
                    if (d > 0x7FFFFFFFll || d < -0x80000000ll) {
                        err = "REL32 overflow (symbol " + symName + ")";
                        return false;
                    }
                    const int32_t v = static_cast<int32_t>(d);
                    memcpy(site, &v, sizeof(v));
                    return true;
                }
                case kRelI386Section: {
                    const uint16_t v = static_cast<uint16_t>(reinterpret_cast<uintptr_t>(target));
                    memcpy(site, &v, sizeof(v));
                    return true;
                }
                case kRelI386SecRel: {
                    const uint32_t v = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(target));
                    memcpy(site, &v, sizeof(v));
                    return true;
                }
                default:
                    err = "unsupported I386 relocation type " + Hex32(rel.Type)
                        + " (symbol " + symName + ")";
                    return false;
                }
            }

        } // namespace

        namespace {

            /// 在宿主模块 ±2GB 内找一块空闲内存(从锚点向高/低交替按分配粒度扫描);
            /// 全空则退回 TOP_DOWN 兜底。
            ///
            /// 为什么必须"近距离": BOF 对 Beacon* 的**直接调用**是 REL32(±2GB 装得下
            /// 才能重定位), 而 Beacon* 实现就在宿主镜像里 —— 映射区必须落在宿主模块
            /// 附近。锚点取本函数地址(本文件编译进 cleat_core, 链入宿主模块)。
            /// `__imp_` 槽位引用与节区互引同样受益(全部落在同一邻域)。
            /// 兜底的 TOP_DOWN 只保证槽位型 BOF 可用(全部映射区互相邻近), 直接调用
            /// 型仍可能 REL32 溢出 —— 由 ApplyRelocation 的范围检查报出来。
            LPVOID NearAlloc(size_t size) {
                const uintptr_t anchor = reinterpret_cast<uintptr_t>(&NearAlloc);
                constexpr uintptr_t kStep = 0x10000;          // 系统分配粒度
                constexpr uintptr_t kRange = 0x7FFF0000ull;   // ~2GB 扫描半径
                for (uintptr_t off = 0; off < kRange; off += kStep) {
                    const uintptr_t cand[2] = { anchor + off, off == 0 ? 0 : anchor - off };
                    for (uintptr_t addr : cand) {
                        if (addr == 0) {
                            continue;
                        }
                        if (LPVOID p = VirtualAlloc(reinterpret_cast<LPVOID>(addr), size,
                            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)) {
                            return p;
                        }
                    }
                }
                return VirtualAlloc(nullptr, size,
                    MEM_COMMIT | MEM_RESERVE | MEM_TOP_DOWN, PAGE_READWRITE);
            }

        } // namespace

        RunResult Execute(const std::vector<uint8_t>& blob,
            const std::string& entry,
            const std::vector<uint8_t>& args) {
            RunResult result;

            // ---- 1. 头部与架构(分配任何内存之前拒掉错误架构) ----
            if (blob.size() < sizeof(Header)) {
                result.error = "blob smaller than COFF header";
                return result;
            }
            const auto* hdr = reinterpret_cast<const Header*>(blob.data());
            if (!native::CoffMachineMatchesHost(blob.data(), blob.size(), result.error)) {
                return result;
            }

            const uint64_t numSections = hdr->NumberOfSections;
            const uint64_t numSymbols = hdr->NumberOfSymbols;   // uint32, 64 位乘法防溢出
            if (numSections == 0 || numSections > kMaxSections) {
                result.error = "bad NumberOfSections";
                return result;
            }
            if (sizeof(Header) + numSections * sizeof(Section) > blob.size()) {
                result.error = "section table out of bounds";
                return result;
            }

            // ---- 2. 符号表 / 字符串表边界 ----
            const uint8_t* symTab = nullptr;
            const uint8_t* strTab = nullptr;
            size_t strTabLen = 0;
            if (numSymbols > 0) {
                const uint64_t symOff = hdr->PointerToSymbolTable;
                const uint64_t symBytes = numSymbols * sizeof(Symbol);
                if (symOff == 0 || symOff + symBytes + 4 > blob.size()) {
                    result.error = "symbol table out of bounds";
                    return result;
                }
                symTab = blob.data() + symOff;
                uint32_t declared = 0;
                memcpy(&declared, symTab + symBytes, 4);
                const size_t remaining = blob.size() - static_cast<size_t>(symOff + symBytes);
                // 长度前缀本身占 4 字节; 声明值截断到 blob 实际剩余(畸形防护)
                strTabLen = (declared >= 4 && declared <= remaining) ? declared
                    : (declared >= 4 ? remaining : 0);
                strTab = symTab + symBytes;
            }

            // ---- 3. 节区映射(RW 拷贝; VirtualAlloc 零页, .bss 无需 memset) ----
            const Section* secs = reinterpret_cast<const Section*>(blob.data() + sizeof(Header));
            uint8_t* mapped[kMaxSections] = {};
            size_t mappedSize[kMaxSections] = {};

            // RAII 收尸: 下面任何一条 early-return 都必须释放已分配内存
            struct Cleanup {
                uint8_t** mapped;
                uint64_t n;
                void* slots = nullptr;
                ~Cleanup() {
                    for (uint64_t i = 0; i < n; ++i) {
                        if (mapped[i]) {
                            VirtualFree(mapped[i], 0, MEM_RELEASE);
                        }
                    }
                    if (slots) {
                        VirtualFree(slots, 0, MEM_RELEASE);
                    }
                }
            } cleanup{ mapped, numSections };

            for (uint64_t i = 0; i < numSections; ++i) {
                // 注意别用 std::max —— windows.h 的 max 宏会碎掉它(本文件风格: 三目)
                const size_t size = secs[i].SizeOfRawData > secs[i].VirtualSize
                    ? secs[i].SizeOfRawData
                    : secs[i].VirtualSize;
                if (size == 0) {
                    continue;
                }
                void* m = NearAlloc(size);
                if (!m) {
                    result.error = "VirtualAlloc(section) failed";
                    return result;
                }
                mapped[i] = static_cast<uint8_t*>(m);
                mappedSize[i] = size;
                if (secs[i].PointerToRawData && secs[i].SizeOfRawData) {
                    const uint64_t rawOff = secs[i].PointerToRawData;
                    if (rawOff + secs[i].SizeOfRawData > blob.size()) {
                        result.error = "section data out of bounds (section " + Hex32(static_cast<uint32_t>(i)) + ")";
                        return result;
                    }
                    memcpy(mapped[i], blob.data() + rawOff, secs[i].SizeOfRawData);
                }
            }

            // ---- 4. 外部函数槽位 ----
            // __imp_X 引用语义 = "取函数指针": 槽位放真实地址, 重定位写槽位地址。
            void* slots = NearAlloc(kMaxExternalSlots * sizeof(void*));
            if (!slots) {
                result.error = "VirtualAlloc(slots) failed";
                return result;
            }
            cleanup.slots = slots;
            void** slotArr = static_cast<void**>(slots);
            unsigned slotCount = 0;

            // ---- 5. 重定位 ----
            for (uint64_t s = 0; s < numSections; ++s) {
                if (secs[s].NumberOfRelocations == 0) {
                    continue;
                }
                const uint64_t rtOff = secs[s].PointerToRelocations;
                if (rtOff == 0
                    || rtOff + static_cast<uint64_t>(secs[s].NumberOfRelocations) * sizeof(Relocation)
                    > blob.size()) {
                    result.error = "relocation table out of bounds (section " + Hex32(static_cast<uint32_t>(s)) + ")";
                    return result;
                }
                const auto* rel = reinterpret_cast<const Relocation*>(blob.data() + rtOff);

                for (uint16_t ri = 0; ri < secs[s].NumberOfRelocations; ++ri, ++rel) {
                    if (rel->SymbolTableIndex >= numSymbols) {
                        result.error = "symbol index out of range";
                        return result;
                    }
                    const auto& sym = *reinterpret_cast<const Symbol*>(
                        symTab + static_cast<size_t>(rel->SymbolTableIndex) * sizeof(Symbol));
                    bool nameOk = false;
                    const std::string rawName = SymbolName(sym, strTab, strTabLen, nameOk);
                    if (!nameOk) {
                        result.error = "symbol name out of bounds";
                        return result;
                    }

                    void* target = nullptr;
                    if (sym.SectionNumber > 0) {
                        // 本节区(或其它节区)内部符号
                        const uint32_t si = static_cast<uint32_t>(sym.SectionNumber) - 1;
                        if (si >= numSections || !mapped[si]) {
                            result.error = "symbol section not mapped (symbol " + rawName + ")";
                            return result;
                        }
                        if (sym.Value > mappedSize[si]) {
                            result.error = "symbol value out of section bounds (symbol " + rawName + ")";
                            return result;
                        }
                        if (rel->Type == kRelAmd64Section || rel->Type == kRelI386Section) {
                            // SECTION 补丁写节区号(1 基), target 复用为编号
                            target = reinterpret_cast<void*>(static_cast<uintptr_t>(sym.SectionNumber));
                        }
                        else if (rel->Type == kRelAmd64SecRel || rel->Type == kRelI386SecRel) {
                            // SECREL 补丁写节区内偏移
                            target = reinterpret_cast<void*>(static_cast<uintptr_t>(sym.Value));
                        }
                        else {
                            target = mapped[si] + sym.Value;
                        }
                    }
                    else if (sym.Value == 0
                        && (sym.StorageClass == kSymClassExternal
                            || sym.StorageClass == kSymClassExternalDef)) {
                        // 外部符号: Beacon API 表 → MODULE$FUNC; __imp_ 引用走槽位
                        const bool imp = IsImpRef(rawName);
                        void* fn = ResolveExternal(imp ? StripImpRef(rawName) : rawName);
                        if (!fn) {
                            result.error = "unresolved external: " + rawName;
                            return result;
                        }
                        if (imp) {
                            if (slotCount >= kMaxExternalSlots) {
                                result.error = "too many external references";
                                return result;
                            }
                            slotArr[slotCount] = fn;
                            target = &slotArr[slotCount];
                            ++slotCount;
                        }
                        else {
                            target = fn;
                        }
                    }
                    else {
                        result.error = "unsupported symbol for relocation: " + rawName;
                        return result;
                    }

                    uint8_t* site = mapped[s] + rel->VirtualAddress;
                    if (!ApplyRelocation(hdr->Machine, *rel, site, mappedSize[s],
                        target, rawName, result.error)) {
                        return result;
                    }
                }
            }

            // ---- 6. 入口符号 ----
            // x64: "go"; x86 mingw 的 C 符号带下划线前缀("go" → "_go"), 两个都接受。
            std::string want = entry.empty() ? "go" : entry;
            std::string underscored;
            const char* candidates[2] = { want.c_str(), nullptr };
            if (hdr->Machine == kMachineI386 && want[0] != '_') {
                underscored = "_" + want;
                candidates[1] = underscored.c_str();
            }
            // 辅助记录槽 —— aux 记录前 4 字节是任意数据(如 FILE 记录的文件名),
            // 恰好拼出 "go" 且 SectionNumber 字节 > 0 时会被误选为入口。
            const Symbol* entrySym = nullptr;
            for (uint64_t i = 0; i < numSymbols && !entrySym;) {
                const auto& sym = *reinterpret_cast<const Symbol*>(
                    symTab + static_cast<size_t>(i) * sizeof(Symbol));
                i += 1 + sym.NumberOfAuxSymbols;
                if (sym.SectionNumber <= 0) {
                    continue;
                }
                bool nameOk = false;
                const std::string nm = SymbolName(sym, strTab, strTabLen, nameOk);
                if (!nameOk) {
                    continue;
                }
                for (int c = 0; c < 2; ++c) {
                    if (candidates[c] && nm == candidates[c]) {
                        entrySym = &sym;
                        break;
                    }
                }
            }
            if (!entrySym) {
                result.error = "entry symbol not found: " + want;
                return result;
            }
            // entrySec 无上界(SectionNumber 最大 32767, mapped 是 kMaxSections=96
            // 的栈数组 → OOB 读约 262KB), entrySym->Value 未与节区大小核对(可跳出
            // 节区间接调用任意地址)。入口必须在节区**内部**(Value 不得 == size)。
            const uint32_t entrySec = static_cast<uint32_t>(entrySym->SectionNumber) - 1;
            if (entrySec >= numSections || !mapped[entrySec]) {
                result.error = "entry section not mapped";
                return result;
            }
            if (entrySym->Value >= mappedSize[entrySec]) {
                result.error = "entry value out of section bounds";
                return result;
            }

            // ---- 7. 收权(先 RW 完成重定位写, 再按 Characteristics 收紧) ----
            // 代码节 → RX; 可写数据(.data/.bss) → 保持 RW; 只读数据 → RO。
            // 收权失败 fail-closed: 宁可拒绝执行也不留 RWX 代码页。
            for (uint64_t i = 0; i < numSections; ++i) {
                if (!mapped[i]) {
                    continue;
                }
                DWORD want_prot = PAGE_READONLY;
                if (secs[i].Characteristics & kScnCntCode) {
                    want_prot = PAGE_EXECUTE_READ;
                }
                else if (secs[i].Characteristics & (kScnMemWrite | kScnCntUninitializedData)) {
                    want_prot = PAGE_READWRITE;
                }
                DWORD old = 0;
                if (!VirtualProtect(mapped[i], mappedSize[i], want_prot, &old)) {
                    result.error = "VirtualProtect(section " + Hex32(static_cast<uint32_t>(i)) + ") failed";
                    return result;
                }
            }

            // ---- 8. 执行(per-execution 上下文挂当前线程; 同步, go() 返回才继续) ----
            // 拷贝一份 args: BOF 可能就地补 NUL/改写(CS 口径参数缓冲可写)。
            std::vector<uint8_t> argsBuf = args;
            char* argPtr = argsBuf.empty() ? nullptr : reinterpret_cast<char*>(argsBuf.data());

            bof::ExecContext ctx;
            struct CtxGuard {
                CtxGuard(bof::ExecContext* c) { bof::SetCurrentContext(c); }
                ~CtxGuard() { bof::SetCurrentContext(nullptr); }
            } ctxGuard(&ctx);

            using GoFn = void (*)(char*, int);
            auto go = reinterpret_cast<GoFn>(mapped[entrySec] + entrySym->Value);
            go(argPtr, static_cast<int>(argsBuf.size()));

            result.output = std::move(ctx.output);
            result.output_truncated = ctx.truncated;
            result.ok = true;
            return result;
        }

    } // namespace coff
} // namespace cleat

#endif // _WIN32
