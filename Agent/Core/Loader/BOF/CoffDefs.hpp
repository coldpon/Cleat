#pragma once
// CoffDefs.hpp — COFF 对象文件结构定义(BOF 支持)。
// 布局口径: MS PE/COFF 对象文件格式规范, 与 AdaptixC2
// src_beacon/beacon/bof_loader.h 的字段一致 —— 这里补齐类型化定义与
// 防护常量, 供 PlatformWin_CoffLoader 使用; 规范本身是紧凑布局, pack(1)。

#pragma once

#include <cstdint>

namespace cleat {
    namespace coff {

#pragma pack(push, 1)

        /// COFF 文件头(20 字节)
        struct Header {
            uint16_t Machine;
            uint16_t NumberOfSections;
            uint32_t TimeDateStamp;
            uint32_t PointerToSymbolTable;
            uint32_t NumberOfSymbols;
            uint16_t SizeOfOptionalHeader;   // 对象文件恒 0
            uint16_t Characteristics;
        };

        /// 节表项(40 字节)
        struct Section {
            char     Name[8];
            uint32_t VirtualSize;
            uint32_t VirtualAddress;
            uint32_t SizeOfRawData;
            uint32_t PointerToRawData;       // 0 = 无原始数据(.bss), 映射后为零页
            uint32_t PointerToRelocations;
            uint32_t PointerToLineNumbers;
            uint16_t NumberOfRelocations;
            uint16_t NumberOfLinenumbers;
            uint32_t Characteristics;
        };

        /// 重定位表项(10 字节)
        struct Relocation {
            uint32_t VirtualAddress;         // 相对节区起始的补丁点
            uint32_t SymbolTableIndex;
            uint16_t Type;
        };

        /// 符号表项(18 字节)
        struct Symbol {
            union {
                char     cName[8];           // ≤8 字节短名(可能非 NUL 结尾!)
                uint32_t dwName[2];          // dwName[0]==0 → dwName[1]=字符串表偏移
            } Name;
            uint32_t Value;                  // 节区内偏移
            int16_t  SectionNumber;          // 1 基; 0 = 外部; -1 = 未定义
            uint16_t Type;
            uint8_t  StorageClass;
            uint8_t  NumberOfAuxSymbols;
        };

#pragma pack(pop)

        // ---- 常量(MS 规范) ----
        enum : uint16_t {
            kMachineI386 = 0x014C,
            kMachineAmd64 = 0x8664,
        };

        enum : uint16_t {
            // AMD64
            kRelAmd64Addr64   = 0x0001,   // 64 位绝对地址
            kRelAmd64Addr32Nb = 0x0003,   // 32 位无基址(RIP 无关)
            kRelAmd64Rel32    = 0x0004,   // 32 位相对(_1.._5 = 0x0005..0x0009, 尾差)
            kRelAmd64Rel32_5  = 0x0009,
            kRelAmd64Section  = 0x000A,   // 16 位节区号
            kRelAmd64SecRel   = 0x000B,   // 32 位节区内偏移
            // I386
            kRelI386Dir32   = 0x0006,     // 32 位绝对
            kRelI386Rel32   = 0x0014,     // 32 位相对
            kRelI386Section = 0x000A,
            kRelI386SecRel  = 0x000B,
        };

        enum : uint8_t {
            kSymClassExternal = 0x02,
            kSymClassExternalDef = 0x03,
        };

        enum : uint32_t {
            kScnCntCode = 0x00000020,
            kScnCntUninitializedData = 0x00000080,
            kScnMemExecute = 0x20000000,
            kScnMemRead = 0x40000000,
            kScnMemWrite = 0x80000000,
        };

        // ---- 防护常量(畸形 COFF 的边界) ----
        inline constexpr unsigned kMaxSections = 96;         // 正常 BOF ≤ 10 节
        inline constexpr unsigned kMaxExternalSlots = 1024;  // 外部函数指针槽上限

    } // namespace coff
} // namespace cleat
