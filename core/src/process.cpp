// SPDX-License-Identifier: MIT
#include "hxvm/process.hpp"

#include <cstdio>
#include <cstring>

#include "hxvm/log.hpp"
#include "hxvm/platform/summary.hpp"

namespace hxvm {

// 由 CPU 后端编码（thunk 立即数要带魔法位，才能和真 syscall 区分开）
u64 ExitMagic();

Process::Process() { qpc_base_ = plat::MonotonicNanos(); }
Process::~Process() = default;

bool Process::Init(const std::string &exe_path, const std::string &asset_root,
                   const std::vector<std::string> &argv) {
    cpu_ = CreateUnicornBackend();
    if (!cpu_) {
        HX_LOGE("process", "CPU 后端创建失败");
        return false;
    }

    // 垫片要先注册，加载器打桩时才知道哪些函数有实现
    RegisterKernel32(shims_);
    RegisterCrt(shims_);
    RegisterUser32(shims_);
    RegisterAdvapi32(shims_);
    RegisterVlc(shims_);
    RegisterSdl2(shims_);
    HX_LOGI("process", "已注册 %zu 个 Win32 垫片", shims_.NativeCount());

    // 资源索引：启动时遍历一次，之后每次路径解析是查表
    fs_.SetRoot(asset_root);
    fs_.SetWorkDir("");
    fs_.EnableMissLog(true);
    const usize nfiles = fs_.BuildIndex();
    HX_LOGI("process", "资源根 %s 索引到 %zu 个文件", asset_root.c_str(), nfiles);

    // thunk 区必须早于映像加载，打桩要用
    if (!cpu_->Map(layout_.thunk_base, layout_.thunk_size,
                   kProtRead | kProtWrite | kProtExec)) {
        HX_LOGE("process", "映射 thunk 区失败");
        return false;
    }

    // 数据导出区（变量槽），必须早于映像加载
    if (!cpu_->Map(layout_.data_base, layout_.data_size, kProtRead | kProtWrite)) {
        HX_LOGE("process", "映射数据导出区失败");
        return false;
    }

    if (!loader_.Load(exe_path, *cpu_, *this, image_)) return false;

    // 栈 / 堆 / TEB / PEB / 出口哨兵
    if (!cpu_->Map(layout_.stack_base, layout_.stack_size, kProtRead | kProtWrite)) {
        HX_LOGE("process", "映射栈失败");
        return false;
    }
    if (!cpu_->Map(layout_.heap_base, layout_.heap_size, kProtRead | kProtWrite)) {
        HX_LOGE("process", "映射堆失败");
        return false;
    }
    if (!cpu_->Map(layout_.teb_base, 0x1000, kProtRead | kProtWrite)) return false;
    if (!cpu_->Map(layout_.peb_base, 0x2000, kProtRead | kProtWrite)) return false;
    if (!cpu_->Map(layout_.exit_base, 0x1000, kProtRead | kProtExec)) return false;

    heap_.Init(cpu_->memory(), layout_.heap_base, layout_.heap_size);

    // 出口哨兵：movabs r10, magic; syscall
    {
        const u64 magic = ExitMagic();
        u8 code[12] = {0x49, 0xBA, 0, 0, 0, 0, 0, 0, 0, 0, 0x0F, 0x05};
        memcpy(code + 2, &magic, 8);
        cpu_->memory().Write(layout_.exit_base, code, sizeof(code));
    }

    if (!SetupStack()) return false;
    if (!SetupPebTeb(argv)) return false;

    // x86_64 Windows 的 TEB 是 gs:[0x30]，必须设基址
    cpu_->SetGsBase(layout_.teb_base);

    cpu_->SetShimDispatcher([this](u64 slot) { HandleShim(slot); });

    cpu_->SetReg(Reg::RIP, image_.image_base + image_.entry_rva);
    HX_LOGI("process", "准备就绪，入口 %#llx",
            static_cast<unsigned long long>(image_.image_base + image_.entry_rva));
    return true;
}

bool Process::SetupStack() {
    const u64 rsp = layout_.stack_base + layout_.stack_size - 0x200;
    // 返回地址指向出口哨兵：main/WinMain 一旦 return，就会被后端识别为正常退出
    cpu_->memory().Write64(rsp, layout_.exit_base);
    cpu_->SetReg(Reg::RSP, rsp);
    return true;
}

bool Process::SetupPebTeb(const std::vector<std::string> &argv) {
    GuestMemory &m = cpu_->memory();
    const u64 teb = layout_.teb_base;
    const u64 peb = layout_.peb_base;

    // TEB（x64 布局常见字段）
    m.Write64(teb + 0x30, teb);        // NtTib.Self
    m.Write64(teb + 0x60, peb);        // ProcessEnvironmentBlock
    m.Write32(teb + 0x68, 0);          // LastErrorValue

    // PEB
    m.Write8(peb + 0x02, 0);           // BeingDebugged
    m.Write64(peb + 0x10, image_.image_base);   // ImageBaseAddress
    m.Write64(peb + 0x18, 0);          // Ldr（暂不伪造模块链表）

    // RTL_USER_PROCESS_PARAMETERS，放在 PEB 后面那页
    const u64 pp = peb + 0x1000;
    m.Write64(peb + 0x20, pp);
    m.Write64(pp + 0x10, 0);           // ConsoleHandle
    m.Write64(pp + 0x20, 0);           // StandardInput
    m.Write64(pp + 0x28, 1);           // StandardOutput（假句柄，够 CRT 用）
    m.Write64(pp + 0x30, 2);           // StandardError

    // 命令行：CRT 会拿它填 argv
    std::string cmdline = "\"" + image_.path + "\"";
    for (const auto &a : argv) {
        cmdline += " ";
        cmdline += a;
    }
    if (cpu_->Map(layout_.cmdline_base, 0x1000, kProtRead | kProtWrite)) {
        m.WriteCString(layout_.cmdline_base, cmdline);
    }
    (void)cmdline;
    return true;
}

void Process::HandleShim(u64 slot) {
    ShimCall &c = shim_call_;
    c.proc = this;
    c.cpu = cpu_.get();
    c.mem = &cpu_->memory();
    c.args[0] = cpu_->GetReg(Reg::RCX);
    c.args[1] = cpu_->GetReg(Reg::RDX);
    c.args[2] = cpu_->GetReg(Reg::R8);
    c.args[3] = cpu_->GetReg(Reg::R9);
    for (int i = 0; i < 4; i++) c.xmm[i] = cpu_->GetXmm(i);
    c.caller_sp = cpu_->GetReg(Reg::RSP);
    shims_.Dispatch(slot, c);
}

int Process::Run(usize instr_budget) {
    for (;;) {
        const u64 rip = cpu_->GetReg(Reg::RIP);
        const CpuResult r = cpu_->Run(rip, 0, instr_budget);

        if (r.kind == CpuResult::Exited) {
            exited_ = true;
            exit_code_ = static_cast<int>(r.exit_code & 0xFFFFFFFFull);
            HX_LOGI("process", "来宾退出，退出码 %d", exit_code_);
            break;
        }
        if (r.kind == CpuResult::Fault) {
            HX_LOGE("process", "来宾故障：%s @ %#llx %s", r.detail,
                    static_cast<unsigned long long>(r.fault_addr),
                    DescribeAddress(r.fault_addr).c_str());
            for (u64 a : TraceBuffer()) {
                HX_LOGE("process", "  轨迹 %#llx %s", static_cast<unsigned long long>(a),
                        DescribeAddress(a).c_str());
            }
            exited_ = true;
            exit_code_ = -1;
            break;
        }
        if (r.kind == CpuResult::Completed || r.kind == CpuResult::Stopped) break;
    }
    return exit_code_;
}

bool Process::RunSlice(usize instr_budget) {
    const u64 rip = cpu_->GetReg(Reg::RIP);
    const CpuResult r = cpu_->Run(rip, 0, instr_budget);

    if (r.kind == CpuResult::Exited) {
        exited_ = true;
        exit_code_ = static_cast<int>(r.exit_code & 0xFFFFFFFFull);
        return false;
    }
    if (r.kind == CpuResult::Fault) {
        HX_LOGE("process", "来宾故障：%s @ %#llx", r.detail,
                static_cast<unsigned long long>(r.fault_addr));
        exited_ = true;
        exit_code_ = -1;
        return false;
    }
    return true;
}

std::vector<u64> Process::TraceBuffer() const { return cpu_->TraceBuffer(); }

std::string Process::DescribeAddress(u64 addr) const {
    const u64 tb = layout_.thunk_base;
    if (addr >= tb && addr < tb + layout_.thunk_size) {
        const u64 slot = (addr - tb) / kThunkSize;
        return "（垫片 thunk：" + shims_.SlotTag(slot) + "）";
    }
    if (addr >= layout_.exit_base && addr < layout_.exit_base + 0x1000) {
        return "（出口哨兵）";
    }
    if (addr >= layout_.data_base && addr < layout_.data_base + layout_.data_size) {
        return "（数据导出槽 #" + std::to_string((addr - layout_.data_base) / 16) + "）";
    }
    if (addr >= image_.image_base && addr < image_.image_base + image_.size_of_image) {
        return "（映像内 RVA " + std::to_string(addr - image_.image_base) + "）";
    }
    return "";
}

void Process::RequestFrameDone() { cpu_->RequestStop(); }

// ── 句柄 ──────────────────────────────────────────────────────────────

u64 Process::NewHandle(Handle h) {
    const u64 v = next_handle_++;
    handles_[v] = h;
    return v;
}

Handle *Process::GetHandle(u64 h) {
    auto it = handles_.find(h);
    return it == handles_.end() ? nullptr : &it->second;
}

bool Process::CloseHandle(u64 h) {
    return handles_.erase(h) > 0;
}

// ── 时间 ──────────────────────────────────────────────────────────────

u64 Process::Qpc() const { return (plat::MonotonicNanos() - qpc_base_) / 100; }

u32 Process::TickCount() const {
    return static_cast<u32>((plat::MonotonicNanos() - qpc_base_) / 1000000ull);
}

// ── 输出 ──────────────────────────────────────────────────────────────

void Process::ConsoleWrite(const std::string &s) {
    if (console_sink_) {
        console_sink_(s);
    } else {
        fputs(s.c_str(), stdout);
        fflush(stdout);
    }
}

void Process::SetConsoleSink(std::function<void(const std::string &)> sink) {
    console_sink_ = std::move(sink);
}

}  // namespace hxvm
