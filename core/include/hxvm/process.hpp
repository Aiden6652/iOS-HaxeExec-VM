// SPDX-License-Identifier: MIT
// 一个来宾进程的全部状态：地址空间、PE 映像、垫片表、句柄表、时钟。
#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "hxvm/basics.hpp"
#include "hxvm/cpu.hpp"
#include "hxvm/filesystem.hpp"
#include "hxvm/guest.hpp"
#include "hxvm/loader.hpp"
#include "hxvm/registry.hpp"
#include "hxvm/shim.hpp"

namespace hxvm {

// 来宾地址空间布局（避开 PE 常驻的 0x140000000 附近）
struct Layout {
    u64 image_base = 0;               // 由 PE 头决定
    u64 stack_base = 0x7FF0'0000'0000ull;
    u64 stack_size = 8 << 20;
    u64 heap_base = 0x2000'0000'0000ull;
    u64 heap_size = 512 << 20;
    u64 thunk_base = 0x3000'0000'0000ull;   // IAT 打桩用的 thunk 区
    u64 thunk_size = 1 << 20;
    // 导入表里有一部分是「数据变量」而不是函数（msvcrt 的 _fmode/__initenv 等）。
    // 它们不能被打桩成代码，得在来宾内存里开真正的变量槽。
    u64 data_base = 0x3000'0010'0000ull;
    u64 data_size = 1 << 20;
    u64 teb_base = 0x4000'0000'0000ull;
    u64 peb_base = 0x4000'0010'0000ull;
    u64 exit_base = 0x7000'0000'0000ull;   // 程序入口返回到的哨兵页
    u64 env_base = 0x5000'0000'0000ull;
    u64 cmdline_base = 0x6000'0000'0000ull;
};

struct Handle {
    enum Kind { kFile, kHeap, kKey, kModule, kEvent, kThread, kMutex, kDummy } kind = kDummy;
    void *host = nullptr;             // 宿主侧对象（FILE* / 堆标记 / ...）
    std::string name;                 // 调试用
    bool is_console = false;
    u64 guest_addr = 0;               // 来宾堆块（如需）
};

class Process {
public:
    Process();
    ~Process();

    // 加载映像。asset_root = iOS 沙盒里游戏资源的位置
    bool Init(const std::string &exe_path, const std::string &asset_root,
              const std::vector<std::string> &argv);

    // 一直跑到 ExitProcess。给 CLI 用。
    int Run(usize instr_budget = 0);

    // 跑到指令预算用完就交还控制权。给 iOS 宿主的 CADisplayLink 用：
    // 一帧跑一片，别把主线程堵死。
    bool RunSlice(usize instr_budget);

    // 帧同步的真正机制：来宾调用 present（SDL_RenderPresent / SDL_GL_SwapWindow）
    // 就说明这一帧画完了，立刻停掉本片，把控制权还给宿主去上屏。
    // 比「每帧跑固定指令数」好得多 —— 后者每帧该跑多少全靠猜。
    void RequestFrameDone();

    // ── 子组件 ──────────────────────────────────────────────────────
    ICpuBackend &cpu() { return *cpu_; }
    GuestMemory &mem() { return cpu_->memory(); }
    ShimTable &shims() { return shims_; }
    FileSystem &fs() { return fs_; }
    Registry &reg() { return reg_; }
    GuestHeap &heap() { return heap_; }
    PEImage &image() { return image_; }
    const Layout &layout() const { return layout_; }

    // ── 句柄 ────────────────────────────────────────────────────────
    u64 NewHandle(Handle h);
    Handle *GetHandle(u64 h);
    bool CloseHandle(u64 h);

    // ── 时间 ────────────────────────────────────────────────────────
    u64 Qpc() const;
    u64 QpcFrequency() const { return 10'000'000ull; }   // 100ns 分辨率，和 Windows 一致
    u32 TickCount() const;

    // ── 控制台 / 输出 ────────────────────────────────────────────────
    void ConsoleWrite(const std::string &s);
    void SetConsoleSink(std::function<void(const std::string &)> sink);

    // LastError（Windows 语义：成功不清零，失败才置）
    void SetLastError(u32 e) { last_error_ = e; }
    u32 LastError() const { return last_error_; }

    // 退出
    bool exited() const { return exited_; }
    int exit_code() const { return exit_code_; }

    // 指令级 trace：崩溃时能把「最后走过的路」打出来
    void EnableInstructionTrace(bool on) { cpu_->EnableInstructionTrace(on); }
    std::vector<u64> TraceBuffer() const;

    // 把来宾地址翻译成人能读的东西（地址落在 thunk 区时能直接指名到导入函数）
    std::string DescribeAddress(u64 addr) const;

    // TEB / PEB
    u64 teb() const { return layout_.teb_base; }
    u64 peb() const { return layout_.peb_base; }

private:
    bool SetupStack();
    bool SetupPebTeb(const std::vector<std::string> &argv);
    // 垫片分发入口（由 CPU 后端在遇到 thunk 时回调）
    void HandleShim(u64 slot);

    std::unique_ptr<ICpuBackend> cpu_;
    ShimTable shims_;
    FileSystem fs_;
    Registry reg_;
    GuestHeap heap_;
    PEImage image_;
    Layout layout_;
    PELoader loader_;

    std::unordered_map<u64, Handle> handles_;
    u64 next_handle_ = 0x40;

    u32 last_error_ = 0;
    bool exited_ = false;
    int exit_code_ = 0;
    std::function<void(const std::string &)> console_sink_;
    u64 qpc_base_ = 0;

    // 每次垫片调用复用同一个对象，避免热路径上反复构造
    ShimCall shim_call_;
};

}  // namespace hxvm
