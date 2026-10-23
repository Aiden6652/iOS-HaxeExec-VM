// SPDX-License-Identifier: MIT
// KERNEL32 垫片 —— 文件、堆、时间、模块、字符串。
//
// 其中「文件」这一块是本项目最要命的一处：Windows 路径不区分大小写且用 `\`，
// iOS 区分大小写且用 `/`。所有路径都必须先过 FileSystem::Resolve，
// 否则资源加载必然落空。
#include "hxvm/shim.hpp"

#include <cstdio>
#include <cstring>

#include "hxvm/log.hpp"
#include "hxvm/platform/summary.hpp"
#include "hxvm/process.hpp"

namespace hxvm {
namespace {

// ── Win32 常量 ────────────────────────────────────────────────────────
constexpr u64 kInvalidHandleValue = ~0ull;
constexpr u32 kErrorSuccess = 0;
constexpr u32 kErrorFileNotFound = 2;
constexpr u32 kErrorPathNotFound = 3;
constexpr u32 kErrorAccessDenied = 5;
constexpr u32 kErrorInvalidHandle = 6;
constexpr u32 kErrorNotEnoughMemory = 8;
constexpr u32 kErrorHandleEof = 38;
constexpr u32 kErrorAlreadyExists = 183;
constexpr u32 kErrorNoMoreFiles = 18;

constexpr u32 kGenericRead = 0x80000000u;
constexpr u32 kGenericWrite = 0x40000000u;

constexpr u32 kCreateNew = 1;
constexpr u32 kCreateAlways = 2;
constexpr u32 kOpenExisting = 3;
constexpr u32 kOpenAlways = 4;
constexpr u32 kTruncateExisting = 5;

constexpr u32 kFileAttributeDirectory = 0x10;
constexpr u32 kFileAttributeNormal = 0x80;

constexpr u32 kFileBegin = 0;
constexpr u32 kFileCurrent = 1;
constexpr u32 kFileEnd = 2;

constexpr u32 kPageReadwrite = 0x04;
constexpr u32 kMemCommit = 0x1000;
constexpr u32 kMemReserve = 0x2000;
constexpr u32 kMemRelease = 0x8000;

// ── 字符串转换 ────────────────────────────────────────────────────────
std::string WToUtf8(const std::u16string &w) {
    std::string out;
    for (char16_t ch : w) {
        if (ch < 0x80) {
            out.push_back(static_cast<char>(ch));
        } else if (ch < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (ch >> 6)));
            out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (ch >> 12)));
            out.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
        }
    }
    return out;
}

std::u16string Utf8ToW(const std::string &s) {
    std::u16string out;
    for (usize i = 0; i < s.size();) {
        u8 c = static_cast<u8>(s[i]);
        if (c < 0x80) {
            out.push_back(static_cast<char16_t>(c));
            i += 1;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
            out.push_back(static_cast<char16_t>(((c & 0x1F) << 6) |
                                               (static_cast<u8>(s[i + 1]) & 0x3F)));
            i += 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
            out.push_back(static_cast<char16_t>(((c & 0x0F) << 12) |
                                               ((static_cast<u8>(s[i + 1]) & 0x3F) << 6) |
                                               (static_cast<u8>(s[i + 2]) & 0x3F)));
            i += 3;
        } else {
            out.push_back(static_cast<char16_t>(c));
            i += 1;
        }
    }
    return out;
}

// 取文件名部分（Windows 语义同时认 / 和 \）
std::string BaseName(const std::string &p) {
    usize pos = p.find_last_of("/\\");
    return pos == std::string::npos ? p : p.substr(pos + 1);
}

std::string DirName(const std::string &p) {
    usize pos = p.find_last_of("/\\");
    return pos == std::string::npos ? std::string() : p.substr(0, pos);
}

void Ret0(ShimCall &c) { c.Ret(0); }
void Ret1(ShimCall &c) { c.Ret(1); }

// ── 文件 ──────────────────────────────────────────────────────────────

// 打开或创建。返回宿主 FILE* 包装成的句柄。
u64 OpenFileCommon(ShimCall &c, const std::string &win_path, u32 access, u32 creation,
                   bool is_console) {
    Process &p = *c.proc;
    std::string real = p.fs().Resolve(win_path);

    const bool want_create = creation == kCreateNew || creation == kCreateAlways ||
                             creation == kOpenAlways;
    if (real.empty()) {
        if (!want_create) {
            p.SetLastError(kErrorFileNotFound);
            return kInvalidHandleValue;
        }
        // 要创建：用规范化后的路径直接落盘（父目录必须已存在）
        std::string norm = p.fs().Resolve(win_path);
        if (norm.empty()) {
            // 父目录也不在索引里 —— 退化到「资源根 + 规范化路径」
            std::string rel = win_path;
            for (char &ch : rel) if (ch == '\\') ch = '/';
            if (rel.size() >= 2 && rel[1] == ':') rel = rel.substr(2);
            while (!rel.empty() && rel.front() == '/') rel.erase(rel.begin());
            norm = p.fs().root().empty() ? rel : p.fs().root() + "/" + rel;
        }
        real = norm;
    }

    const char *mode = "rb";
    if (creation == kCreateAlways || creation == kTruncateExisting) {
        mode = "wb+";
    } else if (creation == kCreateNew) {
        if (FILE *probe = fopen(real.c_str(), "rb")) {
            fclose(probe);
            p.SetLastError(kErrorAlreadyExists);
            return kInvalidHandleValue;
        }
        mode = "wb+";
    } else if (creation == kOpenAlways) {
        mode = "ab+";
    } else if (access & kGenericWrite) {
        mode = "rb+";
    }

    FILE *f = fopen(real.c_str(), mode);
    if (!f) {
        p.SetLastError(kErrorFileNotFound);
        return kInvalidHandleValue;
    }

    Handle h;
    h.kind = Handle::kFile;
    h.host = f;
    h.name = win_path;
    h.is_console = is_console;
    p.SetLastError(kErrorSuccess);
    return p.NewHandle(h);
}

void shim_CreateFileA(ShimCall &c) {
    c.RetHandle(OpenFileCommon(c, c.ArgStrA(0), static_cast<u32>(c.Arg(1)),
                               static_cast<u32>(c.Arg(4)), false));
}

void shim_CreateFileW(ShimCall &c) {
    c.RetHandle(OpenFileCommon(c, WToUtf8(c.ArgStrW(0)), static_cast<u32>(c.Arg(1)),
                               static_cast<u32>(c.Arg(4)), false));
}

void shim_ReadFile(ShimCall &c) {
    Process &p = *c.proc;
    Handle *h = p.GetHandle(c.Arg(0));
    if (!h || h->kind != Handle::kFile) {
        p.SetLastError(kErrorInvalidHandle);
        c.Ret(0);
        return;
    }
    const u64 buf = c.Arg(1);
    const u32 want = static_cast<u32>(c.Arg(2));
    const u64 read_out = c.Arg(3);

    // 控制台句柄没有可读内容
    if (h->is_console) {
        if (read_out) c.mem->Write32(read_out, 0);
        p.SetLastError(kErrorSuccess);
        c.Ret(1);
        return;
    }

    std::vector<u8> tmp(want ? want : 1);
    const size_t got = fread(tmp.data(), 1, want, static_cast<FILE *>(h->host));
    if (got > 0) c.mem->Write(buf, tmp.data(), got);
    if (read_out) c.mem->Write32(read_out, static_cast<u32>(got));
    p.SetLastError(got == want ? kErrorSuccess : kErrorHandleEof);
    c.Ret(1);
}

void shim_WriteFile(ShimCall &c) {
    Process &p = *c.proc;
    Handle *h = p.GetHandle(c.Arg(0));
    if (!h || h->kind != Handle::kFile) {
        // 假句柄 1/2 = stdout/stderr，直接输出到宿主
        std::vector<u8> tmp(static_cast<u32>(c.Arg(2)));
        if (!tmp.empty()) c.mem->Read(c.Arg(1), tmp.data(), tmp.size());
        p.ConsoleWrite(std::string(tmp.begin(), tmp.end()));
        c.Ret(1);
        return;
    }
    const u32 len = static_cast<u32>(c.Arg(2));
    std::vector<u8> tmp(len ? len : 1);
    if (len) c.mem->Read(c.Arg(1), tmp.data(), len);
    if (h->is_console) {
        p.ConsoleWrite(std::string(tmp.begin(), tmp.begin() + len));
    } else {
        fwrite(tmp.data(), 1, len, static_cast<FILE *>(h->host));
    }
    c.Ret(1);
}

void shim_CloseHandle(ShimCall &c) {
    Process &p = *c.proc;
    Handle *h = p.GetHandle(c.Arg(0));
    if (!h) {
        p.SetLastError(kErrorInvalidHandle);
        c.Ret(0);
        return;
    }
    if (h->kind == Handle::kFile && h->host) fclose(static_cast<FILE *>(h->host));
    p.CloseHandle(c.Arg(0));
    p.SetLastError(kErrorSuccess);
    c.Ret(1);
}

void shim_GetFileSize(ShimCall &c) {
    Process &p = *c.proc;
    Handle *h = p.GetHandle(c.Arg(0));
    if (!h || h->kind != Handle::kFile) {
        p.SetLastError(kErrorInvalidHandle);
        c.Ret(0xFFFFFFFFu);
        return;
    }
    FILE *f = static_cast<FILE *>(h->host);
    const long save = ftell(f);
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    fseek(f, save, SEEK_SET);
    const u64 high = c.Arg(1);
    if (high) c.mem->Write32(high, 0);
    c.Ret(static_cast<u32>(sz));
}

void shim_GetFileSizeEx(ShimCall &c) {
    Process &p = *c.proc;
    Handle *h = p.GetHandle(c.Arg(0));
    if (!h || h->kind != Handle::kFile) {
        p.SetLastError(kErrorInvalidHandle);
        c.Ret(0);
        return;
    }
    FILE *f = static_cast<FILE *>(h->host);
    const long save = ftell(f);
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    fseek(f, save, SEEK_SET);
    c.mem->Write64(c.Arg(1), static_cast<u64>(sz));
    c.Ret(1);
}

void shim_SetFilePointer(ShimCall &c) {
    Process &p = *c.proc;
    Handle *h = p.GetHandle(c.Arg(0));
    if (!h || h->kind != Handle::kFile) {
        p.SetLastError(kErrorInvalidHandle);
        c.Ret(0xFFFFFFFFu);
        return;
    }
    const i64 dist = static_cast<i64>(static_cast<i32>(c.Arg(1)));
    const u32 method = static_cast<u32>(c.Arg(3));
    FILE *f = static_cast<FILE *>(h->host);
    int whence = SEEK_SET;
    if (method == kFileCurrent) whence = SEEK_CUR;
    else if (method == kFileEnd) whence = SEEK_END;
    fseek(f, static_cast<long>(dist), whence);
    c.Ret(static_cast<u32>(ftell(f)));
}

void shim_SetFilePointerEx(ShimCall &c) {
    Process &p = *c.proc;
    Handle *h = p.GetHandle(c.Arg(0));
    if (!h || h->kind != Handle::kFile) {
        p.SetLastError(kErrorInvalidHandle);
        c.Ret(0);
        return;
    }
    const i64 dist = static_cast<i64>(c.Arg(1));
    const u32 method = static_cast<u32>(c.Arg(3));
    FILE *f = static_cast<FILE *>(h->host);
    int whence = SEEK_SET;
    if (method == kFileCurrent) whence = SEEK_CUR;
    else if (method == kFileEnd) whence = SEEK_END;
    fseek(f, static_cast<long>(dist), whence);
    if (c.Arg(2)) c.mem->Write64(c.Arg(2), static_cast<u64>(ftell(f)));
    c.Ret(1);
}

void shim_GetFileAttributesA(ShimCall &c) {
    Process &p = *c.proc;
    const std::string real = p.fs().Resolve(c.ArgStrA(0));
    if (real.empty()) {
        p.SetLastError(kErrorFileNotFound);
        c.Ret(0xFFFFFFFFu);
        return;
    }
    c.Ret(kFileAttributeNormal);
}

void shim_GetFileAttributesW(ShimCall &c) {
    Process &p = *c.proc;
    const std::string real = p.fs().Resolve(WToUtf8(c.ArgStrW(0)));
    if (real.empty()) {
        p.SetLastError(kErrorFileNotFound);
        c.Ret(0xFFFFFFFFu);
        return;
    }
    c.Ret(kFileAttributeNormal);
}

// FindFirstFile / FindNextFile：把整个目录项列表一次性缓存到宿主侧
struct FindState {
    std::vector<std::string> entries;
    usize index = 0;
};

void WriteFindData(ShimCall &c, u64 addr, const std::string &name, bool is_dir) {
    GuestMemory &m = *c.mem;
    u64 o = addr;
    m.Write32(o, is_dir ? kFileAttributeDirectory : kFileAttributeNormal);
    o += 4;
    m.Fill(o, 0, 8 * 3);          // 三个 FILETIME
    o += 24;
    m.Write32(o, 0);              // nFileSizeHigh
    o += 4;
    m.Write32(o, 0);              // nFileSizeLow（游戏一般不靠它判断存在性）
    o += 4;
    m.Write32(o, 0);              // dwReserved0
    o += 4;
    m.Write32(o, 0);              // dwReserved1
    o += 4;
    m.WriteCString(o, name);      // cFileName[260]
    o += 260;
    m.Fill(o, 0, 14);             // cAlternateFileName[14]
}

void shim_FindFirstFileA(ShimCall &c) {
    Process &p = *c.proc;
    std::string pattern = c.ArgStrA(0);
    std::string dir = pattern;

    // "assets\songs\*" / "assets\songs\*.*" → 目录部分
    for (char &ch : dir) if (ch == '\\') ch = '/';
    usize slash = dir.find_last_of('/');
    if (slash != std::string::npos) dir = dir.substr(0, slash);

    auto entries = p.fs().ListDir(dir);
    if (entries.empty()) {
        p.SetLastError(kErrorFileNotFound);
        c.Ret(kInvalidHandleValue);
        return;
    }
    // 过滤掉 .* 与通配符
    std::vector<std::string> kept;
    for (const auto &e : entries) {
        if (e == "." || e == "..") continue;
        kept.push_back(e);
    }
    if (kept.empty()) {
        p.SetLastError(kErrorFileNotFound);
        c.Ret(kInvalidHandleValue);
        return;
    }

    FindState *st = new FindState{kept, 0};
    Handle h;
    h.kind = Handle::kDummy;
    h.host = st;
    h.name = pattern;
    const u64 hv = p.NewHandle(h);

    // 顺便把当前目录名返回（Windows 会先返回 "."、".."）
    WriteFindData(c, c.Arg(1), ".", true);
    p.SetLastError(kErrorSuccess);
    c.Ret(hv);
}

void shim_FindNextFileA(ShimCall &c) {
    Process &p = *c.proc;
    Handle *h = p.GetHandle(c.Arg(0));
    if (!h || !h->host) {
        p.SetLastError(kErrorInvalidHandle);
        c.Ret(0);
        return;
    }
    FindState *st = static_cast<FindState *>(h->host);
    if (st->index >= st->entries.size()) {
        p.SetLastError(kErrorNoMoreFiles);
        c.Ret(0);
        return;
    }
    const std::string name = st->entries[st->index++];
    WriteFindData(c, c.Arg(1), name, false);
    c.Ret(1);
}

void shim_FindClose(ShimCall &c) {
    Process &p = *c.proc;
    Handle *h = p.GetHandle(c.Arg(0));
    if (h && h->host) delete static_cast<FindState *>(h->host);
    p.CloseHandle(c.Arg(0));
    c.Ret(1);
}

// ── 堆 / 虚拟内存 ─────────────────────────────────────────────────────

void shim_GetProcessHeap(ShimCall &c) { c.Ret(1); }
void shim_HeapCreate(ShimCall &c) { c.Ret(1); }
void shim_HeapDestroy(ShimCall &c) { c.Ret(1); }

// HeapAlloc(heap, flags, bytes) —— 只有 3 个参数，别按 4 个读
void shim_HeapAlloc(ShimCall &c) {
    Process &p = *c.proc;
    const u32 flags = static_cast<u32>(c.Arg(1));
    const u64 size = c.Arg(2);
    const u64 addr = p.heap().Alloc(static_cast<usize>(size),
                                    (flags & 0x8) != 0);   // HEAP_ZERO_MEMORY
    if (!addr) p.SetLastError(kErrorNotEnoughMemory);
    c.Ret(addr);
}

void shim_HeapReAlloc(ShimCall &c) {
    Process &p = *c.proc;
    const u64 old = c.Arg(2);
    const u64 size = c.Arg(3);
    const u64 addr = p.heap().Alloc(static_cast<usize>(size), false);
    if (addr) {
        // 老内容搬过去（搬旧块大小，保守取小者）
        const usize oldsz = p.heap().SizeOf(old);
        const usize n = std::min<usize>(oldsz, static_cast<usize>(size));
        if (n) {
            std::vector<u8> tmp(n);
            c.mem->Read(old, tmp.data(), n);
            c.mem->Write(addr, tmp.data(), n);
        }
        p.heap().Free(old);
    }
    c.Ret(addr);
}

void shim_HeapFree(ShimCall &c) {
    Process &p = *c.proc;
    c.Ret(p.heap().Free(c.Arg(2)) ? 1 : 0);
}

void shim_HeapSize(ShimCall &c) {
    Process &p = *c.proc;
    c.Ret(static_cast<u64>(p.heap().SizeOf(c.Arg(2))));
}

void shim_VirtualAlloc(ShimCall &c) {
    Process &p = *c.proc;
    const u64 addr = c.Arg(0);
    const u64 size = c.Arg(1);
    const u32 type = static_cast<u32>(c.Arg(2));
    if (type & kMemRelease) {
        c.Ret(0);
        return;
    }
    if (addr) {
        // 指定地址：直接返回（该区域通常已在映像内）
        c.Ret(addr);
        return;
    }
    const u64 got = p.heap().Alloc(static_cast<usize>(size), true);
    c.Ret(got);
}

void shim_VirtualFree(ShimCall &c) { c.Ret(1); }
void shim_VirtualProtect(ShimCall &c) { c.Ret(1); }
void shim_VirtualQuery(ShimCall &c) { c.Ret(0); }

// ── 时间 ──────────────────────────────────────────────────────────────

void shim_QueryPerformanceCounter(ShimCall &c) {
    c.mem->Write64(c.Arg(0), c.proc->Qpc());
    c.Ret(1);
}

void shim_QueryPerformanceFrequency(ShimCall &c) {
    c.mem->Write64(c.Arg(0), c.proc->QpcFrequency());
    c.Ret(1);
}

void shim_GetTickCount(ShimCall &c) { c.Ret(c.proc->TickCount()); }

void shim_GetTickCount64(ShimCall &c) {
    c.Ret(c.proc->TickCount());
}

// FILETIME = 1601-01-01 起的 100ns 计数
void shim_GetSystemTimeAsFileTime(ShimCall &c) {
    const u64 unix_ms = plat::MonotonicNanos() / 1000000ull;
    const u64 ft = 11644473600000ull + unix_ms;   // 粗略：不做墙钟换算
    c.mem->Write64(c.Arg(0), ft * 10000ull);
}

void shim_GetLocalTime(ShimCall &c) {
    // SYSTEMTIME：16 字节全部填 0 会让部分代码算出 1899 年，
    // 这里给个看起来正常的固定时间，够绝大多数用途。
    GuestMemory &m = *c.mem;
    const u64 a = c.Arg(0);
    m.Write16(a + 0, 2024);   // wYear
    m.Write16(a + 2, 1);      // wMonth
    m.Write16(a + 4, 1);      // wDayOfWeek
    m.Write16(a + 6, 1);      // wDay
    m.Write16(a + 8, 12);     // wHour
    m.Write16(a + 10, 0);     // wMinute
    m.Write16(a + 12, 0);     // wSecond
    m.Write16(a + 14, 0);     // wMilliseconds
}

void shim_Sleep(ShimCall &c) {
    // 模拟里阻塞主线程毫无意义，直接空转返回
    (void)c;
    c.Ret(0);
}

// ── 模块 / 进程 ───────────────────────────────────────────────────────

void shim_GetModuleHandleA(ShimCall &c) {
    const std::string name = c.ArgStrA(0);
    if (name.empty()) {
        c.Ret(c.proc->image().image_base);
        return;
    }
    std::string lower = name;
    for (char &ch : lower) if (ch >= 'A' && ch <= 'Z') ch += 32;
    if (lower.find(c.proc->image().name) != std::string::npos ||
        lower.find(".exe") != std::string::npos) {
        c.Ret(c.proc->image().image_base);
        return;
    }
    c.proc->SetLastError(kErrorFileNotFound);
    c.Ret(0);
}

void shim_GetModuleHandleW(ShimCall &c) {
    const std::u16string w = c.ArgStrW(0);
    if (w.empty()) {
        c.Ret(c.proc->image().image_base);
        return;
    }
    c.Ret(0);
}

void shim_GetModuleFileNameA(ShimCall &c) {
    const std::string path = c.proc->image().path;
    const u32 want = static_cast<u32>(c.Arg(2));
    if (want) c.mem->WriteCString(c.Arg(1), path);
    c.Ret(static_cast<u32>(path.size()));
}

void shim_GetModuleFileNameW(ShimCall &c) {
    const std::u16string w = Utf8ToW(c.proc->image().path);
    if (c.Arg(2)) c.mem->WriteWString(c.Arg(1), w);
    c.Ret(static_cast<u32>(w.size()));
}

void shim_LoadLibraryA(ShimCall &c) {
    const std::string name = c.ArgStrA(0);
    HX_LOGW("kernel32", "LoadLibraryA(%s) —— 目标 DLL 不在模拟范围内，返回 0",
            name.c_str());
    c.proc->SetLastError(kErrorFileNotFound);
    c.Ret(0);
}

void shim_LoadLibraryW(ShimCall &c) {
    const std::string name = WToUtf8(c.ArgStrW(0));
    HX_LOGW("kernel32", "LoadLibraryW(%s) —— 不在模拟范围内，返回 0", name.c_str());
    c.proc->SetLastError(kErrorFileNotFound);
    c.Ret(0);
}

void shim_GetProcAddress(ShimCall &c) {
    HX_LOGW("kernel32", "GetProcAddress(%#llx, %s) —— 尚未实现",
            static_cast<unsigned long long>(c.Arg(0)), c.ArgStrA(1).c_str());
    c.Ret(0);
}

void shim_GetCommandLineA(ShimCall &c) { c.Ret(c.proc->layout().cmdline_base); }
void shim_GetCommandLineW(ShimCall &c) { c.Ret(c.proc->layout().cmdline_base); }

void shim_ExitProcess(ShimCall &c) { c.Exit(c.Arg(0)); }

void shim_GetCurrentProcess(ShimCall &c) { c.Ret(1); }
void shim_GetCurrentProcessId(ShimCall &c) { c.Ret(1001); }
void shim_GetCurrentThreadId(ShimCall &c) { c.Ret(2002); }

void shim_GetLastError(ShimCall &c) { c.Ret(c.proc->LastError()); }

void shim_SetLastError(ShimCall &c) {
    c.proc->SetLastError(static_cast<u32>(c.Arg(0)));
    // 同步到 TEB，保证内联读 gs:[...]LastErrorValue 的程序看到一致的值
    c.mem->Write32(c.proc->teb() + 0x68, static_cast<u32>(c.Arg(0)));
}

void shim_GetStdHandle(ShimCall &c) {
    const i32 id = static_cast<i32>(static_cast<u32>(c.Arg(0)));
    if (id == -10) c.Ret(0);        // STD_INPUT_HANDLE
    else if (id == -11) c.Ret(1);   // STD_OUTPUT_HANDLE
    else if (id == -12) c.Ret(2);   // STD_ERROR_HANDLE
    else c.Ret(kInvalidHandleValue);
}

void shim_GetFileType(ShimCall &c) { c.Ret(1); }   // FILE_TYPE_DISK

void shim_IsDebuggerPresent(ShimCall &c) { c.Ret(0); }

void shim_GetSystemInfo(ShimCall &c) {
    GuestMemory &m = *c.mem;
    const u64 a = c.Arg(0);
    u64 o = a;
    m.Write16(o, 9);              // PROCESSOR_ARCHITECTURE_AMD64
    o += 2;
    m.Write16(o, 0);              // wReserved
    o += 2;
    m.Write32(o, 4096);           // dwPageSize
    o += 4;
    m.Write64(o, 0x10000);        // lpMinimumApplicationAddress
    o += 8;
    m.Write64(o, 0x7FFFFFEFFFFFull);  // lpMaximumApplicationAddress
    o += 8;
    m.Write64(o, 0xFF);           // dwActiveProcessorMask
    o += 8;
    m.Write32(o, 4);              // dwNumberOfProcessors
    o += 4;
    m.Write32(o, 8664);           // dwProcessorType
    o += 4;
    m.Write32(o, 65536);          // dwAllocationGranularity
    o += 4;
    m.Write16(o, 6);              // wProcessorLevel
    o += 2;
    m.Write16(o, 0);              // wProcessorRevision
}

void shim_GetVersion(ShimCall &c) { c.Ret(0x0A000000ull | 0x47BB); }

void shim_GetVersionExA(ShimCall &c) {
    GuestMemory &m = *c.mem;
    const u64 a = c.Arg(0);
    m.Write32(a + 0, 0);
    m.Write32(a + 4, 10);          // dwMajorVersion
    m.Write32(a + 8, 0);           // dwMinorVersion
    m.Write32(a + 12, 19045);      // dwBuildNumber
    m.Write32(a + 16, 2);          // VER_PLATFORM_WIN32_NT
    c.Ret(1);
}

void shim_GetVersionExW(ShimCall &c) { shim_GetVersionExA(c); }

void shim_GetACP(ShimCall &c) { c.Ret(1252); }
void shim_GetOEMCP(ShimCall &c) { c.Ret(1252); }
void shim_IsProcessorFeaturePresent(ShimCall &c) { c.Ret(1); }
void shim_GetThreadLocale(ShimCall &c) { c.Ret(0x0409); }
void shim_GetUserDefaultLCID(ShimCall &c) { c.Ret(0x0409); }
void shim_GetLocaleInfoA(ShimCall &c) { c.Ret(0); }
void shim_GetLocaleInfoW(ShimCall &c) { c.Ret(0); }

void shim_OutputDebugStringA(ShimCall &c) {
    HX_LOGI("guest", "OutputDebugString: %s", c.ArgStrA(0).c_str());
    c.Ret(0);
}

void shim_OutputDebugStringW(ShimCall &c) {
    HX_LOGI("guest", "OutputDebugString: %s", WToUtf8(c.ArgStrW(0)).c_str());
    c.Ret(0);
}

// ── 字符串 ────────────────────────────────────────────────────────────

void shim_lstrlenA(ShimCall &c) {
    c.Ret(static_cast<u64>(c.ArgStrA(0).size()));
}

void shim_lstrlenW(ShimCall &c) {
    c.Ret(static_cast<u64>(c.ArgStrW(0).size()));
}

void shim_lstrcpyA(ShimCall &c) {
    const std::string s = c.ArgStrA(1);
    c.mem->WriteCString(c.Arg(0), s);
    c.Ret(c.Arg(0));
}

void shim_lstrcatA(ShimCall &c) {
    const std::string a = c.ArgStrA(0);
    const std::string b = c.ArgStrA(1);
    const std::string out = a + b;
    c.mem->WriteCString(c.Arg(0), out);
    c.Ret(c.Arg(0));
}

void shim_lstrcmpiA(ShimCall &c) {
    std::string a = c.ArgStrA(0), b = c.ArgStrA(1);
    for (char &ch : a) if (ch >= 'A' && ch <= 'Z') ch += 32;
    for (char &ch : b) if (ch >= 'A' && ch <= 'Z') ch += 32;
    c.Ret(static_cast<u64>(a.compare(b)));
}

void shim_WideCharToMultiByte(ShimCall &c) {
    const u32 cp = static_cast<u32>(c.Arg(0));
    const std::u16string w = c.ArgStrW(2);
    // 非 UTF-8/7 之外的代码页不做真实转换，按 Latin-1 折叠，避免非 ASCII 位丢失
    std::string out;
    if (cp == 65001 || cp == 65000) {
        out = WToUtf8(w);
    } else {
        for (char16_t ch : w) out.push_back(static_cast<char>(ch > 0xFF ? '?' : ch));
    }
    if (c.Arg(4)) c.mem->Write(c.Arg(4), out.data(), out.size());
    c.Ret(static_cast<u64>(out.size()));
}

void shim_MultiByteToWideChar(ShimCall &c) {
    const u32 cp = static_cast<u32>(c.Arg(0));
    const std::string s = c.ArgStrA(1);
    const std::u16string out = (cp == 65001 || cp == 65000) ? Utf8ToW(s)
                                                            : Utf8ToW(s);
    if (c.Arg(3)) c.mem->Write(c.Arg(3), out.data(), out.size() * 2);
    c.Ret(static_cast<u64>(out.size()));
}

void shim_CompareStringW(ShimCall &c) {
    const std::u16string a = c.ArgStrW(2);
    const std::u16string b = c.ArgStrW(4);
    c.Ret(a == b ? 2 : (a < b ? 1 : 3));   // CSTR_EQUAL / LESS / GREATER
}

void shim_GetStringTypeW(ShimCall &c) { c.Ret(1); }

// ── 环境 / 目录 ───────────────────────────────────────────────────────

void shim_GetEnvironmentVariableA(ShimCall &c) { c.Ret(0); }
void shim_GetEnvironmentVariableW(ShimCall &c) { c.Ret(0); }
void shim_SetEnvironmentVariableA(ShimCall &c) { c.Ret(1); }
void shim_SetEnvironmentVariableW(ShimCall &c) { c.Ret(1); }

void shim_GetCurrentDirectoryA(ShimCall &c) {
    const std::string wd = c.proc->fs().workdir();
    const u32 size = static_cast<u32>(c.Arg(0));
    if (size) c.mem->WriteCString(c.Arg(1), wd);
    c.Ret(static_cast<u32>(wd.size() + 1));
}

void shim_GetCurrentDirectoryW(ShimCall &c) {
    const std::u16string w = Utf8ToW(c.proc->fs().workdir());
    if (c.Arg(1)) c.mem->WriteWString(c.Arg(1), w);
    c.Ret(static_cast<u32>(w.size() + 1));
}

void shim_SetCurrentDirectoryA(ShimCall &c) {
    c.proc->fs().SetWorkDir(c.ArgStrA(0));
    c.Ret(1);
}

void shim_SetCurrentDirectoryW(ShimCall &c) {
    c.proc->fs().SetWorkDir(WToUtf8(c.ArgStrW(0)));
    c.Ret(1);
}

void shim_GetFullPathNameA(ShimCall &c) {
    const std::string in = c.ArgStrA(0);
    const std::string full = c.proc->fs().Resolve(in);
    const std::string use = full.empty() ? in : full;
    if (c.Arg(2)) c.mem->WriteCString(c.Arg(2), use);
    c.Ret(static_cast<u32>(use.size()));
}

void shim_CreateDirectoryA(ShimCall &c) { c.Ret(1); }
void shim_CreateDirectoryW(ShimCall &c) { c.Ret(1); }
void shim_RemoveDirectoryA(ShimCall &c) { c.Ret(1); }
void shim_RemoveDirectoryW(ShimCall &c) { c.Ret(1); }
void shim_DeleteFileA(ShimCall &c) { c.Ret(1); }
void shim_DeleteFileW(ShimCall &c) { c.Ret(1); }
void shim_CopyFileA(ShimCall &c) { c.Ret(1); }
void shim_CopyFileW(ShimCall &c) { c.Ret(1); }
void shim_MoveFileA(ShimCall &c) { c.Ret(1); }
void shim_MoveFileW(ShimCall &c) { c.Ret(1); }

// ── 同步 / 杂项 ───────────────────────────────────────────────────────

void shim_GetStartupInfoA(ShimCall &c) { c.Ret(0); }
void shim_GetStartupInfoW(ShimCall &c) { c.Ret(0); }
void shim_SetUnhandledExceptionFilter(ShimCall &c) { c.Ret(0); }
void shim_UnhandledExceptionFilter(ShimCall &c) { c.Ret(0); }
void shim_RaiseException(ShimCall &c) { c.Ret(0); }
void shim_FlushFileBuffers(ShimCall &c) { c.Ret(1); }
void shim_SetHandleInformation(ShimCall &c) { c.Ret(1); }
void shim_TerminateProcess(ShimCall &c) { c.Exit(c.Arg(1)); }
void shim_GetProcessAffinityMask(ShimCall &c) {
    c.mem->Write64(c.Arg(1), 0xFF);
    c.mem->Write64(c.Arg(2), 0xFF);
    c.Ret(1);
}
void shim_GetProcessTimes(ShimCall &c) { c.Ret(1); }
void shim_GetThreadTimes(ShimCall &c) { c.Ret(1); }
void shim_DuplicateHandle(ShimCall &c) { c.Ret(1); }
void shim_GetConsoleMode(ShimCall &c) { c.Ret(0); }
void shim_SetConsoleMode(ShimCall &c) { c.Ret(1); }
void shim_GetConsoleCP(ShimCall &c) { c.Ret(1252); }
void shim_SetConsoleCtrlHandler(ShimCall &c) { c.Ret(1); }
void shim_GetConsoleScreenBufferInfo(ShimCall &c) { c.Ret(0); }
void shim_SetConsoleTitleA(ShimCall &c) { c.Ret(1); }
void shim_SetConsoleTitleW(ShimCall &c) { c.Ret(1); }

void shim_CreateMutexA(ShimCall &c) {
    Handle h; h.kind = Handle::kMutex; h.name = c.ArgStrA(2);
    c.Ret(c.proc->NewHandle(h));
}
void shim_CreateMutexW(ShimCall &c) {
    Handle h; h.kind = Handle::kMutex; h.name = WToUtf8(c.ArgStrW(2));
    c.Ret(c.proc->NewHandle(h));
}
void shim_CreateEventA(ShimCall &c) {
    Handle h; h.kind = Handle::kEvent; h.name = c.ArgStrA(2);
    c.Ret(c.proc->NewHandle(h));
}
void shim_CreateEventW(ShimCall &c) {
    Handle h; h.kind = Handle::kEvent; h.name = WToUtf8(c.ArgStrW(2));
    c.Ret(c.proc->NewHandle(h));
}
void shim_WaitForSingleObject(ShimCall &c) { c.Ret(0); }   // WAIT_OBJECT_0
void shim_ReleaseMutex(ShimCall &c) { c.Ret(1); }
void shim_SetEvent(ShimCall &c) { c.Ret(1); }
void shim_ResetEvent(ShimCall &c) { c.Ret(1); }

void shim_CreateThread(ShimCall &c) {
    // 单线程模拟：不真起线程，给个假句柄，主线程自己会跑
    Handle h; h.kind = Handle::kThread;
    c.Ret(c.proc->NewHandle(h));
}
void shim_ExitThread(ShimCall &c) { c.Ret(0); }
void shim_GetExitCodeThread(ShimCall &c) {
    if (c.Arg(1)) c.mem->Write32(c.Arg(1), 0);
    c.Ret(1);
}

// TLS：给 64 个槽，够用
constexpr usize kTlsSlots = 64;
struct TlsTable {
    bool used[kTlsSlots] = {false};
    u64 values[kTlsSlots] = {0};
    u32 next = 1;
};
TlsTable g_tls;

void shim_TlsAlloc(ShimCall &c) {
    for (usize i = g_tls.next; i < kTlsSlots; i++) {
        if (!g_tls.used[i]) {
            g_tls.used[i] = true;
            g_tls.values[i] = 0;
            g_tls.next = static_cast<u32>(i + 1);
            c.Ret(static_cast<u64>(i));
            return;
        }
    }
    c.Ret(0xFFFFFFFFu);
}

void shim_TlsGetValue(ShimCall &c) {
    const u32 idx = static_cast<u32>(c.Arg(0));
    if (idx >= kTlsSlots) { c.Ret(0); return; }
    c.Ret(g_tls.values[idx]);
}

void shim_TlsSetValue(ShimCall &c) {
    const u32 idx = static_cast<u32>(c.Arg(0));
    if (idx >= kTlsSlots) { c.Ret(0); return; }
    g_tls.values[idx] = c.Arg(1);
    c.Ret(1);
}

void shim_TlsFree(ShimCall &c) {
    const u32 idx = static_cast<u32>(c.Arg(0));
    if (idx < kTlsSlots) g_tls.used[idx] = false;
    c.Ret(1);
}

// 互锁：模拟里只有一个线程，直接做普通运算即可
void shim_InterlockedIncrement(ShimCall &c) {
    const u64 a = c.Arg(0);
    const i64 v = static_cast<i64>(c.mem->Read32(a)) + 1;
    c.mem->Write32(a, static_cast<u32>(v));
    c.Ret(static_cast<u64>(v));
}

void shim_InterlockedDecrement(ShimCall &c) {
    const u64 a = c.Arg(0);
    const i64 v = static_cast<i64>(c.mem->Read32(a)) - 1;
    c.mem->Write32(a, static_cast<u32>(v));
    c.Ret(static_cast<u64>(v));
}

void shim_InterlockedExchange(ShimCall &c) {
    const u64 a = c.Arg(0);
    const u32 old = c.mem->Read32(a);
    c.mem->Write32(a, static_cast<u32>(c.Arg(1)));
    c.Ret(old);
}

// MSVC 生成的 exe 会从 vcruntime / api-ms-win-crt 导入这些
void shim_memcpy(ShimCall &c) {
    const usize n = static_cast<usize>(c.Arg(2));
    if (n) {
        std::vector<u8> tmp(n);
        c.mem->Read(c.Arg(1), tmp.data(), n);
        c.mem->Write(c.Arg(0), tmp.data(), n);
    }
    c.Ret(c.Arg(0));
}

void shim_memmove(ShimCall &c) { shim_memcpy(c); }

void shim_memset(ShimCall &c) {
    const usize n = static_cast<usize>(c.Arg(2));
    if (n) c.mem->Fill(c.Arg(0), static_cast<u8>(c.Arg(1)), n);
    c.Ret(c.Arg(0));
}

void shim_memcmp(ShimCall &c) {
    const usize n = static_cast<usize>(c.Arg(2));
    if (!n) { c.Ret(0); return; }
    std::vector<u8> a(n), b(n);
    c.mem->Read(c.Arg(0), a.data(), n);
    c.mem->Read(c.Arg(1), b.data(), n);
    const int r = memcmp(a.data(), b.data(), n);
    c.Ret(static_cast<u64>(r));
}

void shim_strlen(ShimCall &c) { c.Ret(static_cast<u64>(c.ArgStrA(0).size())); }

void shim_strcmp(ShimCall &c) {
    const std::string a = c.ArgStrA(0), b = c.ArgStrA(1);
    c.Ret(static_cast<u64>(a.compare(b)));
}

void shim_malloc(ShimCall &c) {
    c.Ret(c.proc->heap().Alloc(static_cast<usize>(c.Arg(0)), false));
}

void shim_free(ShimCall &c) {
    c.proc->heap().Free(c.Arg(0));
    c.Ret(0);
}

void shim_realloc(ShimCall &c) {
    const u64 size = c.Arg(1);
    const u64 a = c.proc->heap().Alloc(static_cast<usize>(size), false);
    c.Ret(a);
}

void shim_ExpandEnvironmentStringsA(ShimCall &c) {
    const std::string in = c.ArgStrA(0);
    if (c.Arg(1)) c.mem->WriteCString(c.Arg(1), in);
    c.Ret(static_cast<u32>(in.size() + 1));
}

}  // namespace

void RegisterKernel32(ShimTable &t) {
    const char *k = "kernel32.dll";

    // 文件
    t.Register(k, "CreateFileA", shim_CreateFileA);
    t.Register(k, "CreateFileW", shim_CreateFileW);
    t.Register(k, "ReadFile", shim_ReadFile);
    t.Register(k, "WriteFile", shim_WriteFile);
    t.Register(k, "CloseHandle", shim_CloseHandle);
    t.Register(k, "GetFileSize", shim_GetFileSize);
    t.Register(k, "GetFileSizeEx", shim_GetFileSizeEx);
    t.Register(k, "SetFilePointer", shim_SetFilePointer);
    t.Register(k, "SetFilePointerEx", shim_SetFilePointerEx);
    t.Register(k, "GetFileAttributesA", shim_GetFileAttributesA);
    t.Register(k, "GetFileAttributesW", shim_GetFileAttributesW);
    t.Register(k, "GetFileAttributesExA", shim_GetFileAttributesA);
    t.Register(k, "GetFileAttributesExW", shim_GetFileAttributesW);
    t.Register(k, "FindFirstFileA", shim_FindFirstFileA);
    t.Register(k, "FindNextFileA", shim_FindNextFileA);
    t.Register(k, "FindClose", shim_FindClose);
    t.Register(k, "GetFileType", shim_GetFileType);
    t.Register(k, "CreateDirectoryA", shim_CreateDirectoryA);
    t.Register(k, "CreateDirectoryW", shim_CreateDirectoryW);
    t.Register(k, "RemoveDirectoryA", shim_RemoveDirectoryA);
    t.Register(k, "RemoveDirectoryW", shim_RemoveDirectoryW);
    t.Register(k, "DeleteFileA", shim_DeleteFileA);
    t.Register(k, "DeleteFileW", shim_DeleteFileW);
    t.Register(k, "CopyFileA", shim_CopyFileA);
    t.Register(k, "CopyFileW", shim_CopyFileW);
    t.Register(k, "MoveFileA", shim_MoveFileA);
    t.Register(k, "MoveFileW", shim_MoveFileW);
    t.Register(k, "FlushFileBuffers", shim_FlushFileBuffers);
    t.Register(k, "SetHandleInformation", shim_SetHandleInformation);
    t.Register(k, "GetFullPathNameA", shim_GetFullPathNameA);
    t.Register(k, "GetCurrentDirectoryA", shim_GetCurrentDirectoryA);
    t.Register(k, "GetCurrentDirectoryW", shim_GetCurrentDirectoryW);
    t.Register(k, "SetCurrentDirectoryA", shim_SetCurrentDirectoryA);
    t.Register(k, "SetCurrentDirectoryW", shim_SetCurrentDirectoryW);

    // 堆 / 虚拟内存
    t.Register(k, "GetProcessHeap", shim_GetProcessHeap);
    t.Register(k, "HeapCreate", shim_HeapCreate);
    t.Register(k, "HeapDestroy", shim_HeapDestroy);
    t.Register(k, "HeapAlloc", shim_HeapAlloc);
    t.Register(k, "HeapReAlloc", shim_HeapReAlloc);
    t.Register(k, "HeapFree", shim_HeapFree);
    t.Register(k, "HeapSize", shim_HeapSize);
    t.Register(k, "VirtualAlloc", shim_VirtualAlloc);
    t.Register(k, "VirtualFree", shim_VirtualFree);
    t.Register(k, "VirtualProtect", shim_VirtualProtect);
    t.Register(k, "VirtualQuery", shim_VirtualQuery);
    t.Register(k, "malloc", shim_malloc);
    t.Register(k, "free", shim_free);
    t.Register(k, "realloc", shim_realloc);

    // 时间
    t.Register(k, "QueryPerformanceCounter", shim_QueryPerformanceCounter);
    t.Register(k, "QueryPerformanceFrequency", shim_QueryPerformanceFrequency);
    t.Register(k, "GetTickCount", shim_GetTickCount);
    t.Register(k, "GetTickCount64", shim_GetTickCount64);
    t.Register(k, "GetSystemTimeAsFileTime", shim_GetSystemTimeAsFileTime);
    t.Register(k, "GetLocalTime", shim_GetLocalTime);
    t.Register(k, "GetSystemTime", shim_GetLocalTime);
    t.Register(k, "Sleep", shim_Sleep);

    // 模块 / 进程
    t.Register(k, "GetModuleHandleA", shim_GetModuleHandleA);
    t.Register(k, "GetModuleHandleW", shim_GetModuleHandleW);
    t.Register(k, "GetModuleFileNameA", shim_GetModuleFileNameA);
    t.Register(k, "GetModuleFileNameW", shim_GetModuleFileNameW);
    t.Register(k, "LoadLibraryA", shim_LoadLibraryA);
    t.Register(k, "LoadLibraryW", shim_LoadLibraryW);
    t.Register(k, "LoadLibraryExA", shim_LoadLibraryA);
    t.Register(k, "LoadLibraryExW", shim_LoadLibraryW);
    t.Register(k, "GetProcAddress", shim_GetProcAddress);
    t.Register(k, "GetCommandLineA", shim_GetCommandLineA);
    t.Register(k, "GetCommandLineW", shim_GetCommandLineW);
    t.Register(k, "ExitProcess", shim_ExitProcess);
    t.Register(k, "TerminateProcess", shim_TerminateProcess);
    t.Register(k, "GetCurrentProcess", shim_GetCurrentProcess);
    t.Register(k, "GetCurrentProcessId", shim_GetCurrentProcessId);
    t.Register(k, "GetCurrentThreadId", shim_GetCurrentThreadId);
    t.Register(k, "GetLastError", shim_GetLastError);
    t.Register(k, "SetLastError", shim_SetLastError);
    t.Register(k, "GetStdHandle", shim_GetStdHandle);
    t.Register(k, "IsDebuggerPresent", shim_IsDebuggerPresent);
    t.Register(k, "GetSystemInfo", shim_GetSystemInfo);
    t.Register(k, "GetNativeSystemInfo", shim_GetSystemInfo);
    t.Register(k, "GetVersion", shim_GetVersion);
    t.Register(k, "GetVersionExA", shim_GetVersionExA);
    t.Register(k, "GetVersionExW", shim_GetVersionExW);
    t.Register(k, "GetACP", shim_GetACP);
    t.Register(k, "GetOEMCP", shim_GetOEMCP);
    t.Register(k, "IsProcessorFeaturePresent", shim_IsProcessorFeaturePresent);
    t.Register(k, "GetThreadLocale", shim_GetThreadLocale);
    t.Register(k, "GetUserDefaultLCID", shim_GetUserDefaultLCID);
    t.Register(k, "GetLocaleInfoA", shim_GetLocaleInfoA);
    t.Register(k, "GetLocaleInfoW", shim_GetLocaleInfoW);
    t.Register(k, "OutputDebugStringA", shim_OutputDebugStringA);
    t.Register(k, "OutputDebugStringW", shim_OutputDebugStringW);
    t.Register(k, "GetStartupInfoA", shim_GetStartupInfoA);
    t.Register(k, "GetStartupInfoW", shim_GetStartupInfoW);
    t.Register(k, "SetUnhandledExceptionFilter", shim_SetUnhandledExceptionFilter);
    t.Register(k, "UnhandledExceptionFilter", shim_UnhandledExceptionFilter);
    t.Register(k, "RaiseException", shim_RaiseException);
    t.Register(k, "GetProcessAffinityMask", shim_GetProcessAffinityMask);
    t.Register(k, "GetProcessTimes", shim_GetProcessTimes);
    t.Register(k, "GetThreadTimes", shim_GetThreadTimes);
    t.Register(k, "DuplicateHandle", shim_DuplicateHandle);
    t.Register(k, "ExpandEnvironmentStringsA", shim_ExpandEnvironmentStringsA);

    // 环境
    t.Register(k, "GetEnvironmentVariableA", shim_GetEnvironmentVariableA);
    t.Register(k, "GetEnvironmentVariableW", shim_GetEnvironmentVariableW);
    t.Register(k, "SetEnvironmentVariableA", shim_SetEnvironmentVariableA);
    t.Register(k, "SetEnvironmentVariableW", shim_SetEnvironmentVariableW);

    // 字符串
    t.Register(k, "lstrlenA", shim_lstrlenA);
    t.Register(k, "lstrlenW", shim_lstrlenW);
    t.Register(k, "lstrcpyA", shim_lstrcpyA);
    t.Register(k, "lstrcatA", shim_lstrcatA);
    t.Register(k, "lstrcmpiA", shim_lstrcmpiA);
    t.Register(k, "WideCharToMultiByte", shim_WideCharToMultiByte);
    t.Register(k, "MultiByteToWideChar", shim_MultiByteToWideChar);
    t.Register(k, "CompareStringW", shim_CompareStringW);
    t.Register(k, "GetStringTypeW", shim_GetStringTypeW);

    // 同步 / 线程
    t.Register(k, "CreateMutexA", shim_CreateMutexA);
    t.Register(k, "CreateMutexW", shim_CreateMutexW);
    t.Register(k, "CreateEventA", shim_CreateEventA);
    t.Register(k, "CreateEventW", shim_CreateEventW);
    t.Register(k, "WaitForSingleObject", shim_WaitForSingleObject);
    t.Register(k, "ReleaseMutex", shim_ReleaseMutex);
    t.Register(k, "SetEvent", shim_SetEvent);
    t.Register(k, "ResetEvent", shim_ResetEvent);
    t.Register(k, "CreateThread", shim_CreateThread);
    t.Register(k, "ExitThread", shim_ExitThread);
    t.Register(k, "GetExitCodeThread", shim_GetExitCodeThread);
    t.Register(k, "TlsAlloc", shim_TlsAlloc);
    t.Register(k, "TlsGetValue", shim_TlsGetValue);
    t.Register(k, "TlsSetValue", shim_TlsSetValue);
    t.Register(k, "TlsFree", shim_TlsFree);
    t.Register(k, "InterlockedIncrement", shim_InterlockedIncrement);
    t.Register(k, "InterlockedDecrement", shim_InterlockedDecrement);
    t.Register(k, "InterlockedExchange", shim_InterlockedExchange);

    // 临界区：模拟里只有一个线程，直接 no-op
    t.Register(k, "InitializeCriticalSection", Ret1);
    t.Register(k, "InitializeCriticalSectionAndSpinCount", Ret1);
    t.Register(k, "EnterCriticalSection", Ret0);
    t.Register(k, "LeaveCriticalSection", Ret0);
    t.Register(k, "DeleteCriticalSection", Ret0);
    t.Register(k, "IsDBCSLeadByteEx", Ret0);
    t.Register(k, "IsValidCodePage", Ret1);
    t.Register(k, "GetStringTypeA", Ret1);

    // 控制台
    t.Register(k, "GetConsoleMode", shim_GetConsoleMode);
    t.Register(k, "SetConsoleMode", shim_SetConsoleMode);
    t.Register(k, "GetConsoleCP", shim_GetConsoleCP);
    t.Register(k, "SetConsoleCtrlHandler", shim_SetConsoleCtrlHandler);
    t.Register(k, "GetConsoleScreenBufferInfo", shim_GetConsoleScreenBufferInfo);
    t.Register(k, "SetConsoleTitleA", shim_SetConsoleTitleA);
    t.Register(k, "SetConsoleTitleW", shim_SetConsoleTitleW);

    // MSVC runtime 的常见导入（mingw 静态链，MSVC 的 exe 会走这里）
    for (const char *dll : {"vcruntime140.dll", "msvcrt.dll", "api-ms-win-crt-runtime-l1-1-0.dll",
                            "api-ms-win-crt-string-l1-1-0.dll"}) {
        t.Register(dll, "memcpy", shim_memcpy);
        t.Register(dll, "memmove", shim_memmove);
        t.Register(dll, "memset", shim_memset);
        t.Register(dll, "memcmp", shim_memcmp);
        t.Register(dll, "strlen", shim_strlen);
        t.Register(dll, "strcmp", shim_strcmp);
    }
}

}  // namespace hxvm
