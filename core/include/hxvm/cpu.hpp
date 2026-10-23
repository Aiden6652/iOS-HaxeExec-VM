// SPDX-License-Identifier: MIT
// CPU 后端抽象。
//
// 基准数据（bench/RESULTS.md）表明：折损来自 Unicorn 的 soft-MMU 访存路径，
// 不是翻译器本身（纯寄存器代码只有 1.1x）。所以执行核心必须可整体替换 ——
// 后面换成「来宾内存直接映射」的 dynarec 时，垫片层一行都不用改。
#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "hxvm/basics.hpp"
#include "hxvm/guest.hpp"

namespace hxvm {

enum class Reg {
    RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI,
    R8, R9, R10, R11, R12, R13, R14, R15, RIP,
};

// 一次执行的结果
struct CpuResult {
    enum Kind {
        Completed,   // 正常执行到 until 地址
        Exited,     // 来宾调用了 ExitProcess / TerminateProcess
        Stopped,    // 被 RequestStop 打断（时间片用完要交还主线程）
        Fault,      // 访存/取指出错
        Timeout,    // 指令数超限（死循环保护）
    };
    Kind kind = Completed;
    u64 exit_code = 0;
    u64 fault_addr = 0;
    const char *detail = "";
};

// 来宾外部函数调用（垫片）的分发回调：
// 由 CPU 后端在遇到垫片 thunk 时调用，slot 是被调用的垫片编号。
using ShimDispatcher = std::function<void(u64 slot)>;

class ICpuBackend {
public:
    virtual ~ICpuBackend() = default;

    // 内存（来宾地址空间）
    virtual GuestMemory &memory() = 0;

    // 寄存器
    virtual u64 GetReg(Reg r) = 0;
    virtual void SetReg(Reg r, u64 v) = 0;
    virtual void SetXmm0(u64 lo, u64 hi) = 0;
    // 读 XMM0..XMM3 的低 64 位。Windows x64 ABI 里浮点参数走 XMM 而不是通用寄存器，
    // 不读这里，所有带 double/float 参数的调用都会拿到垃圾。
    virtual u64 GetXmm(int i) = 0;

    // 内存保护：映射/改保护
    virtual bool Map(u64 addr, usize size, u32 prot) = 0;
    virtual bool Protect(u64 addr, usize size, u32 prot) = 0;
    virtual bool IsMapped(u64 addr) = 0;

    // 执行。begin = 起始地址；until = 执行到该地址停下（0 = 不限）；
    // max_instr = 最多执行多少条来宾指令（0 = 不限，用于时间片轮转）
    virtual CpuResult Run(u64 begin, u64 until = 0, u64 max_instr = 0) = 0;
    virtual void RequestStop() = 0;

    // x86_64 Windows 的 TEB 通过 gs:[0x30] 访问，必须能把 GS 基址设到 TEB 上，
    // 否则编译器内联的 TEB 访问（GetLastError 等）会读到 0 地址而崩。
    virtual void SetGsBase(u64 base) = 0;

    // 来宾程序退出（ExitProcess）
    virtual void RequestExit(u64 code) = 0;

    // 指令级翻译缓存失效（宿主改写了来宾代码后必须调用）
    virtual void FlushTranslationCache(u64 addr, usize size) = 0;

    // 垫片桥接：登记一个有去无回的地址（用来宾不会自然返回的 thunk 收尾）
    virtual void SetShimDispatcher(ShimDispatcher) = 0;

    // 崩溃时用的指令级 trace（只在该开时开，默认关闭：它会让执行慢一个量级）
    virtual void EnableInstructionTrace(bool on) = 0;
    virtual std::vector<u64> TraceBuffer() const = 0;

    // 统计
    virtual u64 instr_counter() const = 0;   // 自上次重置以来的来宾指令数
    virtual void reset_instr_counter() = 0;
};

// 当前可用的后端。先做 Unicorn（唯一现成能在 iOS 上跑 x86_64 的实现），
// 后面换成直映射 dynarec 时新开一个 CreateDirectMapBackend() 即可。
std::unique_ptr<ICpuBackend> CreateUnicornBackend();

}  // namespace hxvm
