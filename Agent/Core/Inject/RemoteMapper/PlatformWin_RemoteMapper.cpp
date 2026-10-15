#include "Core/Inject/RemoteMapper/PlatformWin_RemoteMapper.hpp"

#include "Core/Inject/RemoteMapper/ShcStub.h"

#include <cstring>

namespace cleat {
namespace injection {

    namespace {

        // ---- PE 头常量(只读解析, 与 dll2shc.validate_dll 同口径) ----
        constexpr unsigned short kDosMagic = 0x5A4D;          // 'MZ'
        constexpr unsigned long kNtMagic = 0x00004550;        // 'PE\0\0'
        constexpr unsigned short kMachineX64 = 0x8664;
        constexpr unsigned short kOptMagicPe32Plus = 0x20B;
        constexpr unsigned short kFileCharDll = 0x2000;       // IMAGE_FILE_DLL
        constexpr unsigned char kBootMagic[5] = { 0xE8, 0x00, 0x00, 0x00, 0x00 };

        /// boot 补丁位(dll2shc.pack 同款): rax = blob 基址 + 5(call $+5 返回址)
        constexpr unsigned long kPatchDllOffset = 9;    // lea rcx,[rax+disp32] disp@9
        constexpr unsigned long kPatchDllSize = 14;     // mov edx,imm32     imm@14
        constexpr unsigned long kPatchLoaderOffset = 24; // lea r9,[rax+disp32] disp@24

    } // namespace

    bool PackNativeDllAsShc(const std::vector<uint8_t>& dll,
        std::vector<uint8_t>& out, std::string& err) {
        out.clear();
        // ---- DLL 校验(fail-closed) ----
        if (dll.size() < 0x40 || dll[0] != 'M' || dll[1] != 'Z') {
            err = "not a PE: MZ header missing";
            return false;
        }
        // 最后一次读取(lfanew+216 = OptionalHeader 至 DataDirectory[9] 末) —— 原实
        // 现两处缺陷: ① unsigned long(32 位)在 0xFFFFFFFA..0xFFFFFFFF 加法回绕绕过
        // 边界检查, memcmp 读基址+4GB 处; ② 只查 lfanew+6 却继续读 lfanew+22/24/208,
        // 小 blob + 靠近尾部的 lfanew 即堆越界读。
        const uint64_t lfanew = *reinterpret_cast<const uint32_t*>(dll.data() + 0x3C);
        if (lfanew + 216 > dll.size()
            || std::memcmp(dll.data() + lfanew, "PE\0\0", 4) != 0) {
            err = "not a PE: NT headers missing";
            return false;
        }
        const unsigned short machine =
            *reinterpret_cast<const unsigned short*>(dll.data() + lfanew + 4);
        if (machine != kMachineX64) {
            err = "not an x64 PE";
            return false;
        }
        const unsigned short optMagic =
            *reinterpret_cast<const unsigned short*>(dll.data() + lfanew + 24);
        if (optMagic != kOptMagicPe32Plus) {
            err = "not a PE32+ image";
            return false;
        }
        const unsigned short characteristics =
            *reinterpret_cast<const unsigned short*>(dll.data() + lfanew + 4 + 18);
        if (!(characteristics & kFileCharDll)) {
            err = "not a DLL (IMAGE_FILE_DLL flag missing)";
            return false;
        }
        // TLS 目录拒载(核心加载器同款 fail-closed): DataDirectory[9]
        // = opt + 112(数据目录起点) + 9*8
        {
            const uint64_t optOff = lfanew + 24;
            const unsigned long long tlsRva =
                *reinterpret_cast<const unsigned long long*>(dll.data() + optOff + 184);
            if (tlsRva != 0) {
                err = "DLL has TLS directory (fail-closed, core loader same policy)";
                return false;
            }
        }
        if (dll.size() > 0x8000000ull) {   // 128MB 上限: 防御性(与上传防填充同量级)
            err = "DLL too large";
            return false;
        }

        // ---- 打包: [stub(补丁)][dll] —— 与 dll2shc.pack 逐字节同源 ----
        out.assign(kShcStubLen + dll.size(), 0);
        std::memcpy(out.data(), kShcStub, kShcStubLen);
        if (std::memcmp(out.data(), kBootMagic, sizeof(kBootMagic)) != 0) {
            out.clear();
            err = "stub header stale (boot magic mismatch) — regenerate via gen_shcstub.py";
            return false;
        }
        auto patch32 = [&](unsigned long off, unsigned int v) {
            std::memcpy(out.data() + off, &v, sizeof(v));
        };
        patch32(kPatchDllOffset, static_cast<unsigned int>(kShcStubLen - 5));
        patch32(kPatchDllSize, static_cast<unsigned int>(dll.size()));
        patch32(kPatchLoaderOffset, kShcStubLoaderEntry - 5);
        std::memcpy(out.data() + kShcStubLen, dll.data(), dll.size());
        return true;
    }

    bool PreparePayload(const std::vector<uint8_t>& payload, PayloadKind kind,
        std::vector<uint8_t>& out, std::string& err) {
        switch (kind) {
        case PayloadKind::Blob:
            out = payload;          // 原样透传(第 0 字节 = 入口)
            return true;
        case PayloadKind::NativeDll:
            return PackNativeDllAsShc(payload, out, err);
        default:
            err = "unsupported payload kind";
            return false;
        }
    }

} // namespace injection
} // namespace cleat
