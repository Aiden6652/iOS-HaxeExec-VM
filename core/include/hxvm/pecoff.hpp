// SPDX-License-Identifier: MIT
// PE32+ 头结构。全部 1 字节对齐，直接从文件缓冲 memcpy 上来读。
#pragma once

#include "hxvm/basics.hpp"
#include "hxvm/guest.hpp"

namespace hxvm::pecoff {

#pragma pack(push, 1)

struct DosHeader {
    u16 e_magic;
    u16 e_cblp, e_cp, e_crlc, e_cparhdr, e_minalloc, e_maxalloc;
    u16 e_ss, e_sp, e_csum, e_ip, e_cs, e_lfarlc, e_ovno;
    u16 e_res[4];
    u16 e_oemid, e_oeminfo;
    u16 e_res2[10];
    u32 e_lfanew;
};

struct FileHeader {
    u16 Machine;
    u16 NumberOfSections;
    u32 TimeDateStamp;
    u32 PointerToSymbolTable;
    u32 NumberOfSymbols;
    u16 SizeOfOptionalHeader;
    u16 Characteristics;
};

struct DataDirectory {
    u32 Rva;
    u32 Size;
};

struct OptionalHeader64 {
    u16 Magic;
    u8 MajorLinkerVersion, MinorLinkerVersion;
    u32 SizeOfCode;
    u32 SizeOfInitializedData;
    u32 SizeOfUninitializedData;
    u32 AddressOfEntryPoint;
    u32 BaseOfCode;
    u64 ImageBase;
    u32 SectionAlignment;
    u32 FileAlignment;
    u16 MajorOperatingSystemVersion, MinorOperatingSystemVersion;
    u16 MajorImageVersion, MinorImageVersion;
    u16 MajorSubsystemVersion, MinorSubsystemVersion;
    u32 Win32VersionValue;
    u32 SizeOfImage;
    u32 SizeOfHeaders;
    u32 CheckSum;
    u16 Subsystem;
    u16 DllCharacteristics;
    u64 SizeOfStackReserve;
    u64 SizeOfStackCommit;
    u64 SizeOfHeapReserve;
    u64 SizeOfHeapCommit;
    u32 LoaderFlags;
    u32 NumberOfRvaAndSizes;
};

struct SectionHeader {
    char Name[8];
    u32 VirtualSize;
    u32 VirtualAddress;
    u32 SizeOfRawData;
    u32 PointerToRawData;
    u32 PointerToRelocations;
    u32 PointerToLineNumbers;
    u16 NumberOfRelocations;
    u16 NumberOfLineNumbers;
    u32 Characteristics;
};

struct ImportDescriptor {
    u32 OriginalFirstThunk;
    u32 TimeDateStamp;
    u32 ForwarderChain;
    u32 Name;
    u32 FirstThunk;
};

struct ExportDirectory {
    u32 Characteristics;
    u32 TimeDateStamp;
    u16 MajorVersion, MinorVersion;
    u32 Name;
    u32 Base;
    u32 NumberOfFunctions;
    u32 NumberOfNames;
    u32 AddressOfFunctions;
    u32 AddressOfNames;
    u32 AddressOfNameOrdinals;
};

#pragma pack(pop)

constexpr u16 kDosMagic = 0x5A4D;         // "MZ"
constexpr u32 kPeSignature = 0x00004550;  // "PE\0\0"
constexpr u16 kPe32PlusMagic = 0x20B;
constexpr u16 kMachineAmd64 = 0x8664;

enum DirIndex {
    kDirExport = 0,
    kDirImport = 1,
    kDirResource = 2,
    kDirBaseReloc = 5,
    kDirDebug = 6,
    kDirTLS = 9,
    kDirIAT = 12,
};

constexpr u32 kSecCode = 0x20;
constexpr u32 kSecInitializedData = 0x40;
constexpr u32 kSecUninitializedData = 0x80;
constexpr u32 kSecDiscardable = 0x02000000;
constexpr u32 kSecExecute = 0x20000000;
constexpr u32 kSecRead = 0x40000000;
constexpr u32 kSecWrite = 0x80000000;

inline u32 SecCharacteristicsToProt(u32 c) {
    u32 p = kProtNone;
    if (c & kSecRead) p |= kProtRead;
    if (c & kSecWrite) p |= kProtWrite;
    if (c & kSecExecute) p |= kProtExec;
    if (p == kProtNone) p = kProtRead | kProtWrite;  // .bss 之类的空属性也要能读写
    return p;
}

}  // namespace hxvm::pecoff
