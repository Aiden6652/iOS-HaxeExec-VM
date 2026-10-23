// SPDX-License-Identifier: MIT
#include "hxvm/shim.hpp"

#include <algorithm>
#include <cstring>

#include "hxvm/log.hpp"
#include "hxvm/process.hpp"

namespace hxvm {
namespace {

std::string Lower(const std::string &s) {
    std::string o = s;
    std::transform(o.begin(), o.end(), o.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return o;
}

}  // namespace

// ── ShimCall ──────────────────────────────────────────────────────────

u64 ShimCall::Arg(int i) const {
    if (i < 4) return args[i];
    return StackArg(i - 4);
}

// Windows x64：call 推入返回地址后 RSP 指向它，其上是 32 字节影子空间，
// 之后才是第 5 个参数。
u64 ShimCall::StackArg(int i) const {
    return mem->Read64(caller_sp + 0x28 + 8 * static_cast<u64>(i));
}

double ShimCall::ArgDouble(int i) const {
    u64 bits = (i < 4) ? xmm[i] : StackArg(i - 4);
    double d = 0.0;
    memcpy(&d, &bits, 8);
    return d;
}

std::string ShimCall::ArgStrA(int i, usize max) const {
    u64 p = Arg(i);
    if (!p) return std::string();
    return mem->ReadCString(p, max);
}

std::u16string ShimCall::ArgStrW(int i, usize max) const {
    u64 p = Arg(i);
    if (!p) return std::u16string();
    return mem->ReadWString(p, max);
}

void ShimCall::Ret(u64 v) {
    cpu->SetReg(Reg::RAX, v);
}

void ShimCall::RetHandle(u64 h) {
    cpu->SetReg(Reg::RAX, h);
}

void ShimCall::RetDouble(double d) {
    u64 bits;
    memcpy(&bits, &d, 8);
    cpu->SetXmm0(bits, 0);
}

void ShimCall::RetFloat(float f) {
    u32 bits;
    memcpy(&bits, &f, 4);
    cpu->SetXmm0(static_cast<u64>(bits), 0);
}

void ShimCall::Fail() {
    cpu->SetReg(Reg::RAX, 0);
    if (proc) proc->SetLastError(1);   // ERROR_INVALID_FUNCTION
}

void ShimCall::Exit(u64 code) {
    cpu->RequestExit(code);
}

// ── ShimTable ─────────────────────────────────────────────────────────

u64 ShimTable::Register(const std::string &dll, const std::string &name, ShimFn fn) {
    auto key = std::make_pair(Lower(dll), name);   // 函数名大小写敏感（导出名是精确的）
    auto it = by_name_.find(key);
    if (it != by_name_.end()) return it->second;

    u64 slot = all_.size();
    all_.push_back(std::move(fn));
    native_.push_back(slot);
    names_.push_back(Lower(dll) + "!" + name);
    by_name_[key] = slot;
    return slot;
}

u64 ShimTable::RegisterOrdinal(const std::string &dll, u16 ordinal, ShimFn fn) {
    auto key = std::make_pair(Lower(dll), ordinal);
    auto it = by_ordinal_.find(key);
    if (it != by_ordinal_.end()) return it->second;

    u64 slot = all_.size();
    all_.push_back(std::move(fn));
    native_.push_back(slot);
    names_.push_back(Lower(dll) + "!#" + std::to_string(ordinal));
    by_ordinal_[key] = slot;
    return slot;
}

u64 ShimTable::RegisterMissing(const std::string &dll, const std::string &name) {
    std::string tag = Lower(dll) + "!" + name;
    if (std::find(missing_.begin(), missing_.end(), tag) != missing_.end()) {
        // 已经登记过，复用同一个 slot
        u64 slot;
        if (Lookup(dll, name, slot)) return slot;
    }
    missing_.push_back(tag);
    const u64 slot = Register(dll, name, [tag](ShimCall &c) {
        HX_LOGW("shim", "未实现的导入调用 %s —— 返回 0", tag.c_str());
        c.Ret(0);
    });
    native_.pop_back();   // 桩不算实现
    return slot;
}

bool ShimTable::Lookup(const std::string &dll, const std::string &name, u64 &slot) const {
    auto it = by_name_.find(std::make_pair(Lower(dll), name));
    if (it == by_name_.end()) return false;
    slot = it->second;
    return true;
}

bool ShimTable::LookupOrdinal(const std::string &dll, u16 ordinal, u64 &slot) const {
    auto it = by_ordinal_.find(std::make_pair(Lower(dll), ordinal));
    if (it == by_ordinal_.end()) return false;
    slot = it->second;
    return true;
}

void ShimTable::Dispatch(u64 slot, ShimCall &call) {
    if (slot >= all_.size()) {
        HX_LOGE("shim", "slot %llu 越界（> %zu）", static_cast<unsigned long long>(slot),
                all_.size());
        call.Ret(0);
        return;
    }
    all_[slot](call);
}

// ── thunk 发射 ────────────────────────────────────────────────────────

std::string ShimTable::SlotTag(u64 slot) const {
    if (slot < names_.size()) return names_[slot];
    return "<slot " + std::to_string(slot) + ">";
}

void EmitThunk(GuestMemory &mem, u64 addr, u64 slot) {
    u8 code[kThunkSize] = {
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,   // movabs rax, imm64
        0x0F, 0x05,                            // syscall
        0xC3,                                  // ret
        0x90, 0x90, 0x90,                      // nop 补齐到 16
    };
    memcpy(code + 2, &slot, 8);
    mem.Write(addr, code, kThunkSize);
}

}  // namespace hxvm
