// SPDX-License-Identifier: MIT
// Win32 API 垫片层。
//
// 来宾代码对外部 DLL 的每一次调用，都会被 IAT 打桩重定向到这里。
// 打桩方式：把 IAT 槽位改成一个 16 字节的 thunk
//
//   48 B8 <slot>    movabs rax, slot
//   0F 05           syscall        ← CPU 后端在这里截断并分发
//   C3              ret            ← 回到来宾调用者
//   90 90 90        nop (对齐)
//
// 之所以用 syscall 而不是软中断：Unicorn 对 UC_HOOK_INSN 有专门的 SYSCALL 支持，
// 代价是已知量（基准里测过），且不依赖任何特权指令。
#pragma once

#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "hxvm/basics.hpp"
#include "hxvm/cpu.hpp"
#include "hxvm/guest.hpp"

namespace hxvm {

class Process;

// 一次来宾外部函数调用的上下文。
// 参数按 Windows x64 ABI：前 4 个在 RCX/RDX/R8/R9，其余在栈上 RSP+0x28 起。
struct ShimCall {
    Process *proc = nullptr;
    ICpuBackend *cpu = nullptr;
    GuestMemory *mem = nullptr;
    u64 args[4] = {0, 0, 0, 0};   // rcx, rdx, r8, r9
    u64 xmm[4] = {0, 0, 0, 0};    // xmm0..xmm3 的低 64 位（浮点参数在这里）
    u64 caller_sp = 0;            // 来宾 call 之后的 RSP

    u64 Arg(int i) const;              // 第 i 个参数（0 起），自动区分寄存器/栈
    u64 StackArg(int i) const;         // 第 i 个栈参数（0 = 第 5 个参数）
    double ArgDouble(int i) const;     // 第 i 个参数是浮点时用这个

    // 读参数指向的字符串（A/W 都给，省得每个垫片自己判）
    std::string ArgStrA(int i, usize max = 4096) const;
    std::u16string ArgStrW(int i, usize max = 4096) const;

    // 写结果
    void Ret(u64 v);
    void RetBool(bool ok) { Ret(ok ? 1 : 0); }
    void RetHandle(u64 h);
    void RetDouble(double d);
    void RetFloat(float f);
    void RetPtr(u64 p) { Ret(p); }
    void Fail();                       // 返回 0 并置典型 LastError
    void Exit(u64 code);
};

using ShimFn = std::function<void(ShimCall &)>;

class ShimTable {
public:
    // 注册一个垫片，返回它的 slot 号
    u64 Register(const std::string &dll, const std::string &name, ShimFn fn);
    u64 RegisterOrdinal(const std::string &dll, u16 ordinal, ShimFn fn);
    // 目标 EXE 用到、但我们还没实现的：登记成返回 0 的桩，并记下来
    u64 RegisterMissing(const std::string &dll, const std::string &name);

    bool Lookup(const std::string &dll, const std::string &name, u64 &slot) const;
    bool LookupOrdinal(const std::string &dll, u16 ordinal, u64 &slot) const;

    void Dispatch(u64 slot, ShimCall &call);

    // 出错时按 slot 反查是哪个导入，省得对着地址发呆
    std::string SlotTag(u64 slot) const;

    usize NativeCount() const { return native_.size(); }
    const std::vector<std::string> &missing() const { return missing_; }

private:
    std::vector<ShimFn> all_;
    std::vector<u64> native_;   // 真正实现了的 slot
    std::map<std::pair<std::string, std::string>, u64> by_name_;  // (lower dll, name)
    std::map<std::pair<std::string, u16>, u64> by_ordinal_;
    std::vector<std::string> missing_;
    std::vector<std::string> names_;
};

constexpr usize kThunkSize = 16;

// 在来宾地址空间发射 thunk（必须可写），之后整段翻成可执行
void EmitThunk(GuestMemory &mem, u64 addr, u64 slot);

// 把各 DLL 的垫片注册进表
void RegisterKernel32(ShimTable &t);
void RegisterCrt(ShimTable &t);
void RegisterUser32(ShimTable &t);
void RegisterAdvapi32(ShimTable &t);
void RegisterVlc(ShimTable &t);
void RegisterSdl2(ShimTable &t);

}  // namespace hxvm
