// SPDX-License-Identifier: MIT
// 平台差异层：iOS 的 JIT/W^X 与高精度时钟，其他平台退化到 POSIX。
#pragma once

#include "hxvm/basics.hpp"

#if defined(__APPLE__)
#include <mach/mach_time.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
// pthread_jit_write_protect_np 是私有 API，声明一下即可
extern "C" int pthread_jit_write_protect_np(int enabled);
extern "C" int pthread_jit_write_protect_supported_np(void);
#else
#include <pthread.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#endif

namespace hxvm::plat {

// ── JIT 内存 ──────────────────────────────────────────────────────────
// iOS 上可执行的匿名内存必须带 MAP_JIT，且写入前要先翻转 W^X。
// 这层 API 让上层不需要知道自己在哪个平台。
inline bool HasJitWriteProtect() {
#if defined(__APPLE__) && defined(__arm64__)
    return true;
#else
    return false;
#endif
}

// 申请一块可写、之后可翻成可执行的内存
inline void *ReserveJit(usize size) {
    void *p = nullptr;
#if defined(__APPLE__) && defined(__arm64__)
    p = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0);
#else
    p = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
    return p == MAP_FAILED ? nullptr : p;
}

// 翻转 W^X：writable=true 时可按写、不可执行；false 时可执行、不可写
inline void JitWriteProtect(bool writable) {
#if defined(__APPLE__) && defined(__arm64__)
    // 参数含义：1 = 允许写、禁止执行；0 = 允许执行、禁止写
    pthread_jit_write_protect_np(writable ? 1 : 0);
#else
    (void)writable;
#endif
}

struct JitWriteScope {
    explicit JitWriteScope(bool need) : need_(need && HasJitWriteProtect()) {
        if (need_) JitWriteProtect(true);
    }
    ~JitWriteScope() {
        if (need_) JitWriteProtect(false);
    }
    bool need_;
};

// ── 高精度单调时钟（QueryPerformanceCounter 的底座）──────────────────
inline u64 MonotonicNanos() {
#if defined(__APPLE__)
    static mach_timebase_info_data_t tb = [] {
        mach_timebase_info_data_t t = {0, 0};
        mach_timebase_info(&t);
        return t;
    }();
    u64 t = mach_absolute_time();
    return t * tb.numer / tb.denom;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<u64>(ts.tv_sec) * 1000000000ull + static_cast<u64>(ts.tv_nsec);
#endif
}

// 进程启动到现在的纳秒（给 GetTickCount 之类用）
inline u64 ProcessUptimeNanos() {
    static const u64 t0 = MonotonicNanos();
    return MonotonicNanos() - t0;
}

inline u64 PageSize() {
    static const u64 ps = static_cast<u64>(sysconf(_SC_PAGESIZE));
    return ps;
}

}  // namespace hxvm::plat
