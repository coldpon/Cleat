/**
 * @file PlatformWin_MachineId.cpp
 * @brief Windows 身份源采集: MachineGuid > 卷序列号 > SMBIOS UUID
 * @details
 * 优先级定稿 (极简方案)。直调 Win32 API(采集为低敏只读操作)。
 *  二审修正: SMBIOS Type1 UUID 偏移 0x08(首版误写 0x18)+越界护栏;
 * MachineGuid 注册表加 KEY_WOW64_64KEY(32 位构建防 WOW64 重定向)。
 */
#include "Core/Identity/Platform/IMachineIdOps.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

namespace cleat {
namespace identity {

    namespace {

        /// MachineGuid: HKLM\SOFTWARE\Microsoft\Cryptography(Windows 安装时生成, 系统级唯一)
        std::string ReadMachineGuid() {
            HKEY hKey = nullptr;
            char guid[64] = { 0 };
            DWORD size = sizeof(guid);
            // KEY_WOW64_64KEY: 32 位构建防 WOW64 重定向到 WOW6432Node(读不到主键)
            if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Cryptography",
                0, KEY_READ | KEY_WOW64_64KEY, &hKey) != ERROR_SUCCESS) {
                return {};
            }
            const LONG rc = RegQueryValueExA(hKey, "MachineGuid", nullptr, nullptr,
                reinterpret_cast<LPBYTE>(guid), &size);
            RegCloseKey(hKey);
            if (rc != ERROR_SUCCESS || guid[0] == '\0') return {};
            // AD8(修复 ): 注册表值若恰为 sizeof(guid) 且不含 NUL(管理员可写),
            // std::string(guid) 会越过栈缓冲读到 0 才停 —— 构造前钉死终止符。
            guid[sizeof(guid) - 1] = '\0';
            return std::string(guid);
        }

        /// 系统盘卷序列号(镜像克隆的机器会相同 —— 仅作第二优先)
        std::string ReadVolumeSerial() {
            DWORD serial = 0;
            char sysDir[MAX_PATH] = { 0 };
            if (GetSystemDirectoryA(sysDir, MAX_PATH) == 0) return {};
            const std::string drive = std::string(sysDir).substr(0, 3);   // "C:\"
            if (!GetVolumeInformationA(drive.c_str(), nullptr, 0, &serial,
                nullptr, nullptr, nullptr, 0) || serial == 0) {
                return {};
            }
            char hex[16];
            std::snprintf(hex, sizeof(hex), "%08X", serial);
            return std::string("vol-") + hex;
        }

        /// SMBIOS Type 1 UUID(GetSystemFirmwareTable 'RSMB' 原始解析)
        /// SMBIOS 规范: Type 1(System Info) 的 UUID 位于结构偏移 **0x08**
        /// (首版误写 0x18 —— 读到 wakeup/SKU 区, 语义错误且可能越界; 二审 P0 修正)。
        std::string ReadSmbiosUuid() {
#pragma pack(push, 1)
            struct RawSmbiosData {
                BYTE Used20CallingMethod;
                BYTE SMBIOSMajorVersion;
                BYTE SMBIOSMinorVersion;
                BYTE DmiRevision;
                DWORD Length;
                BYTE SMBIOSTableData[1];
            };
#pragma pack(pop)
            const DWORD size = GetSystemFirmwareTable('RSMB', 0, nullptr, 0);
            if (size < offsetof(RawSmbiosData, SMBIOSTableData)) return {};   // 头部都放不下

            std::vector<BYTE> buf(size);
            if (GetSystemFirmwareTable('RSMB', 0, buf.data(), size) != size) return {};
            const BYTE* p = buf.data() + offsetof(RawSmbiosData, SMBIOSTableData);
            const BYTE* end = buf.data() + size;

            while (p + 4 < end) {
                const BYTE type = p[0];
                const BYTE len = p[1];
                if (len < 4) break;
                if (type == 1 && len >= 0x18) {
                    if (p + 0x08 + 16 > end) break;
                    const BYTE* uuid = p + 0x08;
                    bool allZero = true, allFf = true;
                    for (int i = 0; i < 16; ++i) {
                        if (uuid[i] != 0x00) allZero = false;
                        if (uuid[i] != 0xFF) allFf = false;
                    }
                    if (allZero || allFf) return {};
                    char out[40];
                    std::snprintf(out, sizeof(out),
                        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x%02x%02x",
                        uuid[3], uuid[2], uuid[1], uuid[0],
                        uuid[5], uuid[4], uuid[7], uuid[6],
                        uuid[8], uuid[9], uuid[10], uuid[11],
                        uuid[12], uuid[13], uuid[14], uuid[15]);
                    return std::string("smbios-") + out;
                }
                const BYTE* q = p + len;
                while (q + 1 < end && !(q[0] == 0 && q[1] == 0)) ++q;
                p = q + 2;
            }
            return {};
        }

        std::string ReadHostname() {
            char comp[256] = { 0 };
            DWORD size = sizeof(comp);
            if (GetComputerNameA(comp, &size)) return std::string(comp);
            return {};
        }

    } // namespace

    std::vector<std::string> CollectSourcesWin() {
        std::vector<std::string> out;
        auto add = [&out](const std::string& s) {
            if (!s.empty()) out.push_back(s);
            };
        add(ReadMachineGuid());
        add(ReadVolumeSerial());
        add(ReadSmbiosUuid());
        return out;
    }

    std::string HostnameFallbackWin() { return ReadHostname(); }

} // namespace identity
} // namespace cleat
