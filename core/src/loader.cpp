// SPDX-License-Identifier: MIT
#include "hxvm/loader.hpp"

#include <algorithm>
#include <set>
#include <cstdio>
#include <cstring>

#include "hxvm/log.hpp"
#include "hxvm/process.hpp"
#include "hxvm/shim.hpp"

namespace hxvm {

// 这两件事由 CPU 后端负责编码：thunk 立即数必须带上魔法位，
// 才能和来宾自己发出的真 syscall 区分开。
u64 EncodeShimSlot(u64 slot);
u64 ExitMagic();

namespace {

std::vector<u8> ReadFileAll(const std::string &path) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return {};
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<u8> out(static_cast<usize>(n > 0 ? n : 0));
    if (n > 0) fread(out.data(), 1, out.size(), f);
    fclose(f);
    return out;
}

// 导入表里并不全是函数。msvcrt 会把一批「全局变量」也导出为导入项
// （_fmode / _commode / _errno / __initenv ...）。
// 来宾代码对它们的用法是 `mov rax, [__imp__fmode]` 再 `mov eax, [rax]`，
// 也就是先取变量地址再取值 —— 如果像函数那样打桩成代码，
// CRT 会读到 thunk 的机器码，然后必崩。
bool IsDataExport(const std::string &name) {
    // 注意：_errno 在 mingw 里是函数 `int* _errno(void)` 而不是变量，
    // __pctype_func / _matherr 同样是函数。把它们当变量处理会让 CRT
    // 跳到数据槽上去执行。
    static const std::set<std::string> kDataNames = {
        "__initenv", "_environ", "_wenviron",
        "_fmode", "_commode",
        "_pgmptr", "_wpgmptr",
        "__argv", "__argc", "__wargv", "__wargc",
        "_osver", "_winver", "_winmajor", "_winminor", "_sys_nerr",
        "_daylight", "_timezone", "_tzname", "_dstbias",
        "__badioinfo", "__lc_handle", "__lc_codepage", "___mb_cur_max",
        "_HUGE",
    };
    return kDataNames.count(name) > 0;
}

std::string LowerName(const std::string &s) {
    std::string o = s;
    std::transform(o.begin(), o.end(), o.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return o;
}

std::string TrimExtension(const std::string &s) {
    usize dot = s.find_last_of('.');
    if (dot == std::string::npos) return s;
    return LowerName(s.substr(0, dot));
}

}  // namespace

bool PELoader::Load(const std::string &path, ICpuBackend &cpu, Process &proc, PEImage &out) {
    std::vector<u8> file = ReadFileAll(path);
    if (file.size() < sizeof(pecoff::DosHeader)) {
        HX_LOGE("loader", "文件太小，不是 PE：%s", path.c_str());
        return false;
    }

    const auto dos = rd<pecoff::DosHeader>(file.data());
    if (dos.e_magic != pecoff::kDosMagic) {
        HX_LOGE("loader", "缺少 MZ 头：%s", path.c_str());
        return false;
    }
    if (dos.e_lfanew + 8 > file.size()) {
        HX_LOGE("loader", "e_lfanew 越界");
        return false;
    }

    const u32 pe_off = dos.e_lfanew;
    if (rd<u32>(file.data(), pe_off) != pecoff::kPeSignature) {
        HX_LOGE("loader", "缺少 PE 签名");
        return false;
    }

    opt_off_ = pe_off + 4 + sizeof(pecoff::FileHeader);
    const auto fh = rd<pecoff::FileHeader>(file.data(), pe_off + 4);
    if (fh.Machine != pecoff::kMachineAmd64) {
        HX_LOGE("loader", "不是 x86_64 映像（Machine=%#x），本项目只做 PE32+ x86_64",
                fh.Machine);
        return false;
    }

    const auto opt = rd<pecoff::OptionalHeader64>(file.data(), opt_off_);
    if (opt.Magic != pecoff::kPe32PlusMagic) {
        HX_LOGE("loader", "不是 PE32+ 可选头（Magic=%#x）", opt.Magic);
        return false;
    }

    sec_off_ = opt_off_ + fh.SizeOfOptionalHeader;
    dirs_off_ = opt_off_ + sizeof(pecoff::OptionalHeader64);

    std::vector<pecoff::SectionHeader> secs;
    for (u16 i = 0; i < fh.NumberOfSections; i++) {
        secs.push_back(rd<pecoff::SectionHeader>(file.data(), sec_off_ + i * 40));
    }

    out.path = path;
    out.name = LowerName(path.substr(path.find_last_of("/\\") == std::string::npos
                                        ? 0
                                        : path.find_last_of("/\\") + 1));
    out.image_base = opt.ImageBase;
    out.entry_rva = opt.AddressOfEntryPoint;
    out.size_of_image = opt.SizeOfImage;
    if (opt.SizeOfStackReserve) out.stack_reserve = opt.SizeOfStackReserve;
    if (opt.SizeOfHeapReserve) out.heap_reserve = opt.SizeOfHeapReserve;
    out.sections = secs;

    HX_LOGI("loader", "%s：映像基址 %#llx，入口 RVA %#x，映像 %u 字节，%u 个节",
            out.name.c_str(), static_cast<unsigned long long>(out.image_base),
            out.entry_rva, out.size_of_image, fh.NumberOfSections);

    if (!MapSections(cpu, file, opt, secs, out)) return false;
    if (!PatchImports(cpu, proc, file, opt, secs, out)) return false;
    ReadExports(cpu, file, opt, secs, out);

    // 事后能一眼看出目标 EXE 到底依赖什么
    HX_LOGI("loader", "依赖 DLL 共 %zu 个：", dlls_.size());
    for (const auto &d : dlls_) HX_LOGI("loader", "  - %s", d.c_str());

    return true;
}

bool PELoader::MapSections(ICpuBackend &cpu, const std::vector<u8> &file,
                           const pecoff::OptionalHeader64 &opt,
                           const std::vector<pecoff::SectionHeader> &secs,
                           PEImage &out) {
    const u64 base = out.image_base;
    const u64 map_base = page_down(base);
    const u64 map_size = page_up(out.size_of_image + (base - map_base));

    // 整体按 RWX 映射。理由是游戏里常见热补丁/自修改，按节严格设保护会让
    // 调试期频繁踩到预期外的写保护。真要加固，按节设保护即可。
    if (!cpu.Map(map_base, map_size, kProtRead | kProtWrite | kProtExec)) {
        HX_LOGE("loader", "映射映像区域失败 [%#llx, %#llx)",
                static_cast<unsigned long long>(map_base),
                static_cast<unsigned long long>(map_base + map_size));
        return false;
    }

    // 头
    const usize hdr = std::min<usize>(opt.SizeOfHeaders, file.size());
    if (hdr && !cpu.memory().Write(base, file.data(), hdr)) {
        HX_LOGE("loader", "写入 PE 头失败");
        return false;
    }

    // 各节的原始数据；VirtualSize > SizeOfRawData 的部分（.bss）本来就是 0
    usize mapped = 0;
    for (const auto &s : secs) {
        if (s.SizeOfRawData == 0) continue;
        if (s.PointerToRawData + s.SizeOfRawData > file.size()) continue;
        if (!cpu.memory().Write(base + s.VirtualAddress,
                                file.data() + s.PointerToRawData, s.SizeOfRawData)) {
            HX_LOGE("loader", "写入节 %.8s 失败", s.Name);
            return false;
        }
        mapped++;
    }
    HX_LOGI("loader", "映射完成：%zu/%zu 个节有原始数据", mapped, secs.size());
    return true;
}

bool PELoader::PatchImports(ICpuBackend &cpu, Process &proc, const std::vector<u8> &file,
                            const pecoff::OptionalHeader64 &opt,
                            const std::vector<pecoff::SectionHeader> &, PEImage &out) {
    if (opt.NumberOfRvaAndSizes <= pecoff::kDirImport) return true;

    const auto dd = rd<pecoff::DataDirectory>(file.data(),
                                              dirs_off_ + pecoff::kDirImport * 8);
    if (dd.Rva == 0 || dd.Size == 0) return true;

    GuestMemory &mem = cpu.memory();
    const u64 base = out.image_base;
    const u64 thunk_base = proc.layout().thunk_base;
    data_brk_ = proc.layout().data_base;

    u64 desc = base + dd.Rva;
    usize resolved = 0, missing = 0;

    for (;;) {
        pecoff::ImportDescriptor d;
        d.OriginalFirstThunk = mem.Read32(desc + 0);
        d.TimeDateStamp = mem.Read32(desc + 4);
        d.ForwarderChain = mem.Read32(desc + 8);
        d.Name = mem.Read32(desc + 12);
        d.FirstThunk = mem.Read32(desc + 16);
        if (d.Name == 0 && d.FirstThunk == 0) break;

        std::string dll = mem.ReadCString(base + d.Name);
        std::string dll_lower = LowerName(dll);
        if (std::find(dlls_.begin(), dlls_.end(), dll) == dlls_.end()) dlls_.push_back(dll);

        // 名字表和 IAT 分开时，用名字表遍历、写 IAT
        const u64 name_table = base + (d.OriginalFirstThunk ? d.OriginalFirstThunk : d.FirstThunk);
        const u64 iat = base + d.FirstThunk;

        for (u64 i = 0;; i++) {
            const u64 val = mem.Read64(name_table + i * 8);
            if (val == 0) break;

            const bool by_ordinal = (val >> 63) & 1;
            const u16 ord = static_cast<u16>(val & 0xFFFF);
            std::string fname;

            if (!by_ordinal) {
                // Hint/Name 表项：2 字节 hint + 名字
                fname = mem.ReadCString(base + static_cast<u32>(val & 0xFFFFFFFF) + 2);
            }

            // 数据变量：在来宾内存里开一个真正的槽，IAT 槽位写「槽的地址」
            if (!by_ordinal && IsDataExport(fname)) {
                const u64 cell = data_brk_;
                data_brk_ += 16;
                mem.Write64(cell, 0);
                mem.Write64(iat + i * 8, cell);
                HX_LOGI("loader", "数据导出槽 #%llu = %s（%s 里的变量，不是函数）",
                        (cell - proc.layout().data_base) / 16, fname.c_str(),
                        dll.c_str());

                ImportInfo info;
                info.dll = dll;
                info.func = fname + " (数据变量)";
                info.ordinal = false;
                info.slot = iat + i * 8;
                info.resolved = true;
                imports_.push_back(info);
                continue;
            }

            u64 slot = 0;
            bool found = by_ordinal ? proc.shims().LookupOrdinal(dll, ord, slot)
                                    : proc.shims().Lookup(dll, fname, slot);
            if (!found) {
                const std::string key = by_ordinal ? ("#" + std::to_string(ord)) : fname;
                slot = proc.shims().RegisterMissing(dll, key);
                missing++;
            } else {
                resolved++;
            }

            const u64 thunk_addr = thunk_base + slot * kThunkSize;
            EmitThunk(mem, thunk_addr, EncodeShimSlot(slot));
            mem.Write64(iat + i * 8, thunk_addr);

            ImportInfo info;
            info.dll = dll;
            info.func = fname;
            info.ordinal = by_ordinal;
            info.ordinal_value = ord;
            info.slot = iat + i * 8;
            info.resolved = found;
            imports_.push_back(info);
        }
        desc += sizeof(pecoff::ImportDescriptor);
    }

    HX_LOGI("loader", "导入表：%zu 个已替换，%zu 个尚无实现（后者返回 0）",
            resolved, missing);
    if (missing) {
        HX_LOGW("loader", "未实现的导入清单：");
        for (const auto &m : proc.shims().missing()) HX_LOGW("loader", "  - %s", m.c_str());
    }
    return true;
}

bool PELoader::ReadExports(ICpuBackend &cpu, const std::vector<u8> &file,
                           const pecoff::OptionalHeader64 &opt,
                           const std::vector<pecoff::SectionHeader> &, PEImage &out) {
    if (opt.NumberOfRvaAndSizes <= pecoff::kDirExport) return true;

    const auto dd = rd<pecoff::DataDirectory>(file.data(),
                                              dirs_off_ + pecoff::kDirExport * 8);
    if (dd.Rva == 0 || dd.Size == 0) return true;

    // 直接从文件读（导出表尚未被改写，没必要走来宾内存）
    pecoff::ExportDirectory ed;
    {
        usize off = 0;
        // 先算出导出目录在文件里的位置
        for (const auto &s : out.sections) {
            u32 vsize = s.VirtualSize ? s.VirtualSize : s.SizeOfRawData;
            if (dd.Rva >= s.VirtualAddress && dd.Rva < s.VirtualAddress + vsize) {
                off = s.PointerToRawData + (dd.Rva - s.VirtualAddress);
                break;
            }
        }
        if (!off) return true;
        ed = rd<pecoff::ExportDirectory>(file.data(), off);
        (void)ed;
    }
    // 大多数游戏 EXE 不导出东西，这里只保证结构解析不炸
    return true;
}

std::vector<std::string> PELoader::DllList() const { return dlls_; }

}  // namespace hxvm
