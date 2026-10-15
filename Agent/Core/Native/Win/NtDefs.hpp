#pragma once
// NtDefs.hpp — L1 syscall 层私有类型定义(对齐 phnt 语义, 自带声明避免 winternl.h 缺口)。
// 仅 Windows(见 Syscall.hpp 的 _WIN32 守卫); 本头文件不含任何平台 API 调用。
#include <cstddef>
#include <cstdint>

#ifdef _WIN32

#ifndef NTAPI
#define NTAPI __stdcall
#endif

namespace cleat {
    namespace native {

        using NTSTATUS = long;
        using PS_ATTRIBUTE_NUM = unsigned char;

        constexpr NTSTATUS STATUS_SUCCESS = 0x00000000;
        constexpr NTSTATUS STATUS_INVALID_HANDLE = 0xC0000008;
        constexpr NTSTATUS STATUS_INFO_LENGTH_MISMATCH = 0xC0000004;

        // ---- 进程/线程访问权(v1 所需最小集) ----
        constexpr unsigned long PROCESS_CREATE_PROCESS = 0x0080;
        constexpr unsigned long PROCESS_TERMINATE = 0x0001;
        constexpr unsigned long PROCESS_VM_OPERATION = 0x0008;
        constexpr unsigned long PROCESS_VM_WRITE = 0x0020;
        constexpr unsigned long PROCESS_VM_READ = 0x0010;
        constexpr unsigned long PROCESS_CREATE_THREAD = 0x0002;
        constexpr unsigned long PROCESS_QUERY_INFORMATION = 0x0400;
        constexpr unsigned long THREAD_SET_CONTEXT = 0x0010;
        constexpr unsigned long THREAD_SUSPEND_RESUME = 0x0002;
        constexpr unsigned long THREAD_GET_CONTEXT = 0x0008;

        // ---- OBJECT_ATTRIBUTES / CLIENT_ID(x64 布局) ----
        struct UNICODE_STRING_NT {
            unsigned short Length;
            unsigned short MaximumLength;
            wchar_t* Buffer;
        };

        struct OBJECT_ATTRIBUTES_NT {
            unsigned long Length;
            void* RootDirectory;
            UNICODE_STRING_NT* ObjectName;
            unsigned long Attributes;
            void* SecurityDescriptor;
            void* SecurityQualityOfService;
        };

        struct CLIENT_ID_NT {
            void* UniqueProcess;
            void* UniqueThread;
        };

        // ---- PS_ATTRIBUTE / PS_CREATE_INFO(NtCreateUserProcess) ----
        struct PS_ATTRIBUTE {
            std::uintptr_t Attribute;    // PsAttributeValue(num, thread, input, unknown)
            std::size_t Size;
            union {
                std::uintptr_t Value;
                void* ValuePtr;
            };
            std::size_t* ReturnLength;
        };

        struct PS_ATTRIBUTE_LIST {
            std::size_t TotalLength;
            PS_ATTRIBUTE Attributes[4];   // v1: PARENT_PROCESS + IMAGE_NAME(+2 预留)
        };

        // PS_CREATE_STATE: Initial=0(最小用法: 只填 Size+State, 其余清零)
        // 镜像 AdaptixC2 beacon/defines.h 的 PS_CREATE_INFO(x64 布局逐字段一致)
        struct PS_CREATE_INFO {
            std::size_t Size;
            unsigned long State;          // PsCreateInitialState = 0
            unsigned long Pad;
            struct {
                union {
                    unsigned long InitFlags;
                    struct {
                        unsigned char WriteOutputOnExit : 1;
                        unsigned char DetectManifest : 1;
                        unsigned char IFEOSkipDebugger : 1;
                        unsigned char IFEODoNotPropagateKeyState : 1;
                        unsigned char SpareBits1 : 4;
                        unsigned char SpareBits2 : 8;
                        unsigned short ProhibitedImageCharacteristics : 16;
                    } s1;
                } u1;
                unsigned long AdditionalFileAccess;
            } InitState;
            struct {
                union {
                    unsigned long OutputFlags;
                    struct {
                        unsigned char ProtectedProcess : 1;
                        unsigned char AddressSpaceOverride : 1;
                        unsigned char DevOverrideEnabled : 1;
                        unsigned char ManifestDetected : 1;
                        unsigned char ProtectedProcessLight : 1;
                        unsigned char SpareBits1 : 3;
                        unsigned char SpareBits2 : 8;
                        unsigned short SpareBits3 : 16;
                    } s2;
                } u2;
                void* FileHandle;
                void* SectionHandle;
                std::uint64_t UserProcessParametersNative;
                unsigned long UserProcessParametersWow64;
                unsigned long CurrentParameterFlags;
                std::uint64_t PebAddressNative;
                unsigned long PebAddressWow64;
                // 官方 winnt.h 布局: ManifestAddress 是 ULONG(4B) —— Adaptix 用
                // ULONGLONG 使 sizeof 达 88, 内核按官方 80 解析 → CreateInfo.Size
                // 不匹配 → NtCreateUserProcess 返回 0xC000000D(实测)。
                unsigned long ManifestAddress;
                unsigned long ManifestSize;
            } SuccessState;
        };

        // ---- 属性编号与编码公式(对齐 AdaptixC2 beacon/defines.h, 已验证实现) ----
        // num 在低 16 位; flags: bit16=THREAD bit17=INPUT bit18=ADDITIVE
        constexpr std::uintptr_t PS_ATTRIBUTE_NUMBER_MASK = 0x0000FFFF;
        constexpr std::uintptr_t PS_ATTRIBUTE_THREAD_FLAG = 0x00010000;
        constexpr std::uintptr_t PS_ATTRIBUTE_INPUT_FLAG = 0x00020000;
        constexpr std::uintptr_t PS_ATTRIBUTE_ADDITIVE_FLAG = 0x00040000;

        constexpr PS_ATTRIBUTE_NUM PsAttributeNumParentProcess = 0;
        constexpr PS_ATTRIBUTE_NUM PsAttributeNumImageName = 5;   // ntddk 枚举: ImageName=5, 注释 "in PWSTR"(DirectNtCreateUserProcess 参考工程实测)

        constexpr std::uintptr_t PsAttributeValue(PS_ATTRIBUTE_NUM num, bool thread,
            bool input, bool additive) {
            return (static_cast<std::uintptr_t>(num) & PS_ATTRIBUTE_NUMBER_MASK)
                | (thread ? PS_ATTRIBUTE_THREAD_FLAG : 0)
                | (input ? PS_ATTRIBUTE_INPUT_FLAG : 0)
                | (additive ? PS_ATTRIBUTE_ADDITIVE_FLAG : 0);
        }

        // v1 属性(IMAGE_NAME 的 ValuePtr = 指向 UNICODE_STRING 的指针)
        constexpr std::uintptr_t PS_ATTRIBUTE_PARENT_PROCESS =
            PsAttributeValue(PsAttributeNumParentProcess, false, true, true);
        constexpr std::uintptr_t PS_ATTRIBUTE_IMAGE_NAME =
            PsAttributeValue(PsAttributeNumImageName, false, true, false);

        // RTL_USER_PROCESS_PARAMETERS_NORMALIZED(RtlCreateProcessParametersEx 标志)
        constexpr unsigned long RTL_USER_PROCESS_PARAMETERS_NORMALIZED = 0x1;

        // ---- 创建标志 ----
        constexpr unsigned long PROCESS_CREATE_FLAGS_NONE = 0x0;
        constexpr unsigned long PROCESS_CREATE_FLAGS_SUSPENDED = 0x00000200;   // 创建即挂起(参考工程实测)
        constexpr unsigned long THREAD_CREATE_FLAGS_CREATE_SUSPENDED = 0x00000001;

    } // namespace native
} // namespace cleat

#endif // _WIN32
