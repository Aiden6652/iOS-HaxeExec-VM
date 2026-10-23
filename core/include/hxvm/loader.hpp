// SPDX-License-Identifier: MIT
// PE32+ 加载器：读头、按节映射、重写导入表（IAT 打桩到垫片）、登记导出。
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "hxvm/basics.hpp"
#include "hxvm/cpu.hpp"
#include "hxvm/guest.hpp"
#include "hxvm/pecoff.hpp"

namespace hxvm {

class Process;
class ShimTable;

struct PEImage {
    std::string path;
    std::string name;        // 模块名（小写），给 GetModuleHandle 用
    u64 image_base = 0;
    u64 entry_rva = 0;
    u64 size_of_image = 0;
    u64 stack_reserve = 1 << 20;
    u64 stack_commit = 16 << 10;
    u64 heap_reserve = 1 << 20;
    u64 heap_commit = 16 << 10;
    std::vector<pecoff::SectionHeader> sections;

    std::unordered_map<std::string, u64> export_by_name;
    std::unordered_map<u32, u64> export_by_ordinal;
};

class PELoader {
public:
    // 加载成功返回 true，并把信息填进 out
    bool Load(const std::string &path, ICpuBackend &cpu, Process &proc, PEImage &out);

    // 导入表打桩时记录下来，事后能看出目标 EXE 到底依赖了哪些东西
    struct ImportInfo {
        std::string dll;
        std::string func;
        bool ordinal = false;
        u16 ordinal_value = 0;
        u64 slot = 0;        // IAT 槽位地址
        bool resolved = false;
    };
    const std::vector<ImportInfo> &imports() const { return imports_; }

    // 目标 EXE 依赖了哪些 DLL —— 这是判断「在哪个边界拦截渲染/音频」的依据
    std::vector<std::string> DllList() const;

private:
    bool MapSections(ICpuBackend &cpu, const std::vector<u8> &file,
                     const pecoff::OptionalHeader64 &opt,
                     const std::vector<pecoff::SectionHeader> &secs, PEImage &out);
    bool PatchImports(ICpuBackend &cpu, Process &proc,
                      const std::vector<u8> &file, const pecoff::OptionalHeader64 &opt,
                      const std::vector<pecoff::SectionHeader> &secs, PEImage &out);
    bool ReadExports(ICpuBackend &cpu, const std::vector<u8> &file,
                     const pecoff::OptionalHeader64 &opt,
                     const std::vector<pecoff::SectionHeader> &secs, PEImage &out);

    std::vector<ImportInfo> imports_;
    std::vector<std::string> dlls_;

    // 文件内偏移，各解析步骤共用
    usize opt_off_ = 0;
    usize dirs_off_ = 0;
    usize sec_off_ = 0;

    // 数据导出槽的分配游标
    u64 data_brk_ = 0;
};

}  // namespace hxvm
