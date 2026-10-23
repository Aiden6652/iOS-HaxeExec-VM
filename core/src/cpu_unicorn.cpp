// SPDX-License-Identifier: MIT
// Unicorn 后端。
//
// 它是当前唯一现成能在 iOS(ARM64) 上执行 x86_64 的实现，所以先拿它把整条链路
// 跑通。但基准（bench/RESULTS.md）已经量出它的代价：来宾访存走 soft-MMU，
// 写操作折损 40~80 倍。因此这里只暴露 ICpuBackend 接口，
// 后面换成「来宾内存直接映射」的 dynarec 时，上层（垫片/加载器）一行都不用动。
#include "hxvm/cpu.hpp"

#include <unicorn/unicorn.h>
#include <unicorn/x86.h>

#include <cstring>
#include <map>
#include <vector>

#include "hxvm/log.hpp"
#include "hxvm/platform/summary.hpp"

namespace hxvm {
namespace {

class UnicornBackend;

// Unicorn 2.1.4 的 UC_HOOK_INSN 回调不会把 user_data 传出来
// （实测拿到的是栈上的垃圾，见 git 记录里的排查过程）。
// 所以这里自己维护 uc_engine* → backend 的映射，回调里按 uc 查回对象。
std::map<uc_engine *, UnicornBackend *> &BackendRegistry() {
    static std::map<uc_engine *, UnicornBackend *> m;
    return m;
}

// 垫片 thunk 会把 slot 或上这个魔法值，用来和「来宾自己发的真 syscall」区分开
constexpr u64 kShimMagic = 0x5357000000000000ull;
constexpr u64 kShimSlotMask = 0x0000FFFFFFFFFFFFull;
// 程序入口返回用的哨兵：movabs r10, kExitMagic; syscall
constexpr u64 kExitMagic = 0x53570000DEADBEEFull;

u32 ToUcProt(u32 p) {
    u32 r = UC_PROT_NONE;
    if (p & kProtRead) r |= UC_PROT_READ;
    if (p & kProtWrite) r |= UC_PROT_WRITE;
    if (p & kProtExec) r |= UC_PROT_EXEC;
    return r;
}

int ToUcReg(Reg r) {
    switch (r) {
        case Reg::RAX: return UC_X86_REG_RAX;
        case Reg::RCX: return UC_X86_REG_RCX;
        case Reg::RDX: return UC_X86_REG_RDX;
        case Reg::RBX: return UC_X86_REG_RBX;
        case Reg::RSP: return UC_X86_REG_RSP;
        case Reg::RBP: return UC_X86_REG_RBP;
        case Reg::RSI: return UC_X86_REG_RSI;
        case Reg::RDI: return UC_X86_REG_RDI;
        case Reg::R8: return UC_X86_REG_R8;
        case Reg::R9: return UC_X86_REG_R9;
        case Reg::R10: return UC_X86_REG_R10;
        case Reg::R11: return UC_X86_REG_R11;
        case Reg::R12: return UC_X86_REG_R12;
        case Reg::R13: return UC_X86_REG_R13;
        case Reg::R14: return UC_X86_REG_R14;
        case Reg::R15: return UC_X86_REG_R15;
        case Reg::RIP: return UC_X86_REG_RIP;
    }
    return UC_X86_REG_RAX;
}

class UnicornMemory final : public GuestMemory {
public:
    explicit UnicornMemory(uc_engine *uc) : uc_(uc) {}

    bool Map(u64 addr, usize size, u32 prot) override {
        if (uc_mem_map(uc_, addr, size, ToUcProt(prot)) != UC_ERR_OK) return false;
        regions_.emplace_back(addr, addr + size);
        return true;
    }

    bool Protect(u64 addr, usize size, u32 prot) override {
        return uc_mem_protect(uc_, addr, size, ToUcProt(prot)) == UC_ERR_OK;
    }

    bool IsMapped(u64 addr) override {
        for (const auto &r : regions_) {
            if (addr >= r.first && addr < r.second) return true;
        }
        return false;
    }

    bool Write(u64 addr, const void *data, usize n) override {
        return uc_mem_write(uc_, addr, data, n) == UC_ERR_OK;
    }

    bool Read(u64 addr, void *data, usize n) const override {
        return uc_mem_read(uc_, addr, data, n) == UC_ERR_OK;
    }

    bool Fill(u64 addr, u8 value, usize n) override {
        // 分块，避免一次分配巨大缓冲（.bss 动辄几十 MB）
        std::vector<u8> buf(64 * 1024, value);
        usize done = 0;
        while (done < n) {
            usize chunk = std::min<usize>(buf.size(), n - done);
            if (!Write(addr + done, buf.data(), chunk)) return false;
            done += chunk;
        }
        return true;
    }

private:
    uc_engine *uc_;
    std::vector<std::pair<u64, u64>> regions_;
};

class UnicornBackend final : public ICpuBackend {
public:
    UnicornBackend() {
        if (uc_open(UC_ARCH_X86, UC_MODE_64, &uc_) != UC_ERR_OK) {
            HX_LOGE("cpu", "uc_open 失败");
            return;
        }
        mem_ = std::make_unique<UnicornMemory>(uc_);
        // 全局范围生效：begin>end 表示不限地址
        uc_err e = uc_hook_add(uc_, &hook_, UC_HOOK_INSN,
                               reinterpret_cast<void *>(&UnicornBackend::OnInsn),
                               this, 1, 0, UC_X86_INS_SYSCALL);
        if (e != UC_ERR_OK) {
            HX_LOGE("cpu", "注册 syscall hook 失败：%s", uc_strerror(e));
        }
        ok_ = true;
        BackendRegistry()[uc_] = this;
    }

    ~UnicornBackend() override {
        BackendRegistry().erase(uc_);
        if (uc_) {
            uc_hook_del(uc_, hook_);
            uc_close(uc_);
        }
    }

    bool valid() const { return ok_; }

    GuestMemory &memory() override { return *mem_; }

    u64 GetReg(Reg r) override {
        u64 v = 0;
        uc_reg_read(uc_, ToUcReg(r), &v);
        return v;
    }

    void SetReg(Reg r, u64 v) override { uc_reg_write(uc_, ToUcReg(r), &v); }

    void SetXmm0(u64 lo, u64 hi) override {
        u8 buf[16];
        memcpy(buf, &lo, 8);
        memcpy(buf + 8, &hi, 8);
        uc_reg_write(uc_, UC_X86_REG_XMM0, buf);
    }

    u64 GetXmm(int i) override {
        static const int regs[4] = {UC_X86_REG_XMM0, UC_X86_REG_XMM1,
                                    UC_X86_REG_XMM2, UC_X86_REG_XMM3};
        if (i < 0 || i > 3) return 0;
        u8 buf[16] = {0};
        if (uc_reg_read(uc_, regs[i], buf) != UC_ERR_OK) return 0;
        u64 v = 0;
        memcpy(&v, buf, 8);
        return v;
    }

    bool Map(u64 addr, usize size, u32 prot) override { return mem_->Map(addr, size, prot); }
    bool Protect(u64 addr, usize size, u32 prot) override { return mem_->Protect(addr, size, prot); }
    bool IsMapped(u64 addr) override { return mem_->IsMapped(addr); }

    CpuResult Run(u64 begin, u64 until, u64 max_instr) override {
        pending_stop_ = false;
        pending_exit_ = false;
        exit_code_ = 0;
        uc_err e = uc_emu_start(uc_, begin, until, 0, max_instr);
        return Finish(e);
    }

    void SetGsBase(u64 base) override {
        uc_reg_write(uc_, UC_X86_REG_GS_BASE, &base);
    }

    void RequestStop() override {
        pending_stop_ = true;
        uc_emu_stop(uc_);
    }

    void RequestExit(u64 code) override {
        pending_exit_ = true;
        exit_code_ = code;
        uc_emu_stop(uc_);
    }

    void FlushTranslationCache(u64, usize) override {
        // Unicorn 没有公开的 TB flush 接口；我们的打桩都在执行前完成，暂不需要。
        // 若将来要支持来宾运行时自修改代码，得在后端里补。
    }

    void SetShimDispatcher(ShimDispatcher d) override { dispatcher_ = std::move(d); }

    void EnableInstructionTrace(bool on) override {
        if (on && !trace_hook_) {
            uc_hook_add(uc_, &trace_hook_, UC_HOOK_CODE,
                        reinterpret_cast<void *>(&UnicornBackend::OnCode), nullptr, 1, 0);
        } else if (!on && trace_hook_) {
            uc_hook_del(uc_, trace_hook_);
            trace_hook_ = 0;
        }
    }

    // 环形缓冲必须按「时间顺序」回放，否则拿到的是几百条之前的旧轨迹
    std::vector<u64> TraceBuffer() const override {
        std::vector<u64> out;
        if (trace_.empty()) return out;
        const usize n = trace_.size();
        const usize count = std::min<usize>(n, usize{200});
        for (usize i = count; i >= 1; i--) {
            out.push_back(trace_[(trace_pos_ - i) % n]);
        }
        return out;
    }

    u64 instr_counter() const override { return instr_budget_hint_; }
    void reset_instr_counter() override { instr_budget_hint_ = 0; }

    // 仅供内部：shim 分发
    void HandleInsn();

private:
    static void OnCode(uc_engine *uc, u64 addr, u32 size, void *ud) {
        (void)size;
        (void)ud;
        auto it = BackendRegistry().find(uc);
        if (it == BackendRegistry().end()) return;
        it->second->RecordTrace(addr);
    }

    void RecordTrace(u64 addr) {
        if (trace_.size() < 256) trace_.push_back(addr);
        else trace_[trace_pos_ % trace_.size()] = addr;
        trace_pos_++;
    }

    static void OnInsn(uc_engine *uc, uint32_t insn, void *ud) {
        (void)insn;
        (void)ud;   // Unicorn 不保证把它传过来，别依赖它
        auto it = BackendRegistry().find(uc);
        if (it == BackendRegistry().end()) return;
        it->second->HandleInsn();
    }

    CpuResult Finish(uc_err e) {
        CpuResult r;
        if (pending_exit_) {
            r.kind = CpuResult::Exited;
            r.exit_code = exit_code_;
        } else if (pending_stop_) {
            r.kind = CpuResult::Stopped;
        } else if (e == UC_ERR_OK) {
            r.kind = CpuResult::Completed;
        } else {
            r.kind = CpuResult::Fault;
            r.fault_addr = GetReg(Reg::RIP);
            r.detail = uc_strerror(e);
        }
        return r;
    }

    uc_engine *uc_ = nullptr;
    uc_hook hook_ = 0;
    std::unique_ptr<UnicornMemory> mem_;
    ShimDispatcher dispatcher_;
    bool ok_ = false;
    bool pending_stop_ = false;
    bool pending_exit_ = false;
    u64 exit_code_ = 0;
    u64 instr_budget_hint_ = 0;

    uc_hook trace_hook_ = 0;
    std::vector<u64> trace_;
    usize trace_pos_ = 0;
};

void UnicornBackend::HandleInsn() {
    const u64 rax = GetReg(Reg::RAX);
    const u64 r10 = GetReg(Reg::R10);
    const u64 rip = GetReg(Reg::RIP);

    // 1) 程序入口返回到哨兵 → 正常退出，退出码在 RAX
    if (r10 == kExitMagic) {
        RequestExit(rax & 0xFFFFFFFFull);
        return;
    }

    // 2) 不是我们的 thunk → 来宾自己发的真 syscall，按无操作处理
    if ((rax & ~kShimSlotMask) != kShimMagic) {
        HX_LOGW("cpu", "来宾在 %#llx 直接执行了 syscall（rax=%#llx），按无操作处理",
                static_cast<unsigned long long>(rip), static_cast<unsigned long long>(rax));
        // 不要手动推进 RIP，见下面第 3 点的说明
        return;
    }

    // 3) 垫片调用
    //
    // 【关键】这里绝对不要手动 SetReg(RIP, rip + 2)。
    // Unicorn 在 UC_HOOK_INSN 回调返回后，会自己把 RIP 推进过 syscall 指令（2 字节），
    // 正好落到 thunk 里的 ret 上；我们再 +2 就会跳过 ret，执行流顺着 nop 滑进
    // 下一个 thunk，栈从此失衡。（IC 排查记录：thunk 内 0xb3a→0xb3e 就是这么来的）
    if (dispatcher_) dispatcher_(rax & kShimSlotMask);
}

}  // namespace

std::unique_ptr<ICpuBackend> CreateUnicornBackend() {
    auto b = std::unique_ptr<UnicornBackend>(new UnicornBackend());
    if (!b->valid()) return nullptr;
    return b;
}

// 供 Process 使用：把 slot 编成 thunk 里的立即数
u64 EncodeShimSlot(u64 slot) { return kShimMagic | (slot & kShimSlotMask); }
u64 ExitMagic() { return kExitMagic; }

}  // namespace hxvm
