// SPDX-License-Identifier: MIT
// MSVCRT / VCRUNTIME 垫片。
//
// 目标 EXE 的 C 运行时是从 msvcrt.dll 动态导入的。不把这一层垫上，
// 来宾连 printf 都出不来（__iob_func 返回 0 会让 CRT 往地址 0 写）。
// 这一层里的 printf 格式化是真的做的：否则我们连来宾程序的输出都看不见。
#include "hxvm/shim.hpp"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "hxvm/log.hpp"
#include "hxvm/process.hpp"

namespace hxvm {
namespace {

// MSVCRT 的 FILE 结构（x64）
// _ptr(0x00) _cnt(0x08) _base(0x10) _flag(0x18) _file(0x1C)
// _charbuf(0x20) _bufsiz(0x24) _tmpfname(0x28)  → 48 字节
constexpr usize kFileSize = 48;
u64 g_iob = 0;

u64 EnsureIOB(ShimCall &c) {
    if (g_iob) return g_iob;
    const u64 base = c.proc->heap().Alloc(kFileSize * 3, true);
    for (int i = 0; i < 3; i++) {
        const u64 f = base + i * kFileSize;
        c.mem->Write32(f + 0x18, 0x2);      // _flag = _IOWRT
        c.mem->Write32(f + 0x1C, i);        // _file = 0/1/2
    }
    g_iob = base;
    return base;
}

// 读第 i 个可变参数。
// va_ptr 非 0 时（vfprintf）：从 va_list 指向的栈区按 8 字节槽位读；
// 否则（fprintf）：按 Windows x64 规则，参数 2/3 在 R8/R9，之后进栈。
u64 ReadVararg(ShimCall &c, int i, u64 va_ptr) {
    if (va_ptr) return c.mem->Read64(va_ptr + 8 * static_cast<u64>(i));
    const int abs = i + 2;
    if (abs == 2 || abs == 3) return c.args[abs];
    return c.mem->Read64(c.caller_sp + 0x28 + 8 * (abs - 4));
}

std::string FormatPrintf(ShimCall &c, const std::string &fmt, u64 va_ptr) {
    std::string out;
    char buf[512];
    int arg = 0;

    for (usize i = 0; i < fmt.size();) {
        if (fmt[i] != '%') {
            out.push_back(fmt[i]);
            i++;
            continue;
        }
        i++;   // 吃掉 '%'

        std::string spec = "%";
        while (i < fmt.size() && strchr("-+ #0", fmt[i])) { spec.push_back(fmt[i]); i++; }
        while (i < fmt.size() && std::isdigit(static_cast<u8>(fmt[i]))) {
            spec.push_back(fmt[i]);
            i++;
        }
        if (i < fmt.size() && fmt[i] == '.') {
            spec.push_back('.');
            i++;
            while (i < fmt.size() && std::isdigit(static_cast<u8>(fmt[i]))) {
                spec.push_back(fmt[i]);
                i++;
            }
        }
        bool is_ll = false;
        while (i < fmt.size() && (fmt[i] == 'l' || fmt[i] == 'h' || fmt[i] == 'L' ||
                                  fmt[i] == 'z' || fmt[i] == 'I')) {
            if (fmt[i] == 'l' && i + 1 < fmt.size() && fmt[i + 1] == 'l') {
                is_ll = true;
                spec += "ll";
                i += 2;
            } else if (fmt[i] != 'h') {
                spec.push_back(fmt[i]);
                i++;
            } else {
                i++;   // 'h' 忽略，最后都按 int 处理
            }
        }
        if (i >= fmt.size()) break;

        const char type = fmt[i];
        i++;
        if (type == '%') {
            out.push_back('%');
            continue;
        }

        const u64 raw = ReadVararg(c, arg++, va_ptr);
        const std::string s = spec + type;

        switch (type) {
            case 'd':
            case 'i':
                if (is_ll) snprintf(buf, sizeof buf, "%lld", static_cast<long long>(raw));
                else snprintf(buf, sizeof buf, s.c_str(), static_cast<int>(static_cast<i32>(raw)));
                break;
            case 'u':
            case 'x':
            case 'X':
            case 'o':
                if (is_ll) snprintf(buf, sizeof buf, s.c_str(), raw);
                else snprintf(buf, sizeof buf, s.c_str(), static_cast<unsigned>(raw & 0xFFFFFFFFull));
                break;
            case 'c':
                snprintf(buf, sizeof buf, "%c", static_cast<char>(raw & 0xFF));
                break;
            case 's': {
                std::string v = raw ? c.mem->ReadCString(raw) : "(null)";
                snprintf(buf, sizeof buf, s.c_str(), v.c_str());
                break;
            }
            case 'p':
                snprintf(buf, sizeof buf, "%p", reinterpret_cast<void *>(raw));
                break;
            case 'f':
            case 'F':
            case 'g':
            case 'G':
            case 'e':
            case 'E': {
                double d = 0.0;
                memcpy(&d, &raw, 8);
                snprintf(buf, sizeof buf, s.c_str(), d);
                break;
            }
            default:
                snprintf(buf, sizeof buf, "%%%c", type);
                break;
        }
        out += buf;
    }
    return out;
}

void Emit(ShimCall &c, u64 stream, const std::string &text) {
    // 只区分「输出到哪」，目前一律当控制台
    (void)stream;
    c.proc->ConsoleWrite(text);
}

void shim_fprintf(ShimCall &c) {
    const std::string out = FormatPrintf(c, c.ArgStrA(1), 0);
    Emit(c, c.Arg(0), out);
    c.Ret(out.size());
}

void shim_vfprintf(ShimCall &c) {
    const std::string out = FormatPrintf(c, c.ArgStrA(1), c.Arg(2));
    Emit(c, c.Arg(0), out);
    c.Ret(out.size());
}

void shim_printf(ShimCall &c) {
    const std::string out = FormatPrintf(c, c.ArgStrA(0), 0);
    Emit(c, 1, out);
    c.Ret(out.size());
}

void shim_fwrite(ShimCall &c) {
    const u64 total = c.Arg(1) * c.Arg(2);
    std::vector<u8> buf(static_cast<usize>(total) + 1, 0);
    if (total) c.mem->Read(c.Arg(0), buf.data(), static_cast<usize>(total));
    c.proc->ConsoleWrite(std::string(buf.begin(), buf.begin() + total));
    c.Ret(c.Arg(2));
}

void shim_fputc(ShimCall &c) {
    const char ch = static_cast<char>(c.Arg(0) & 0xFF);
    c.proc->ConsoleWrite(std::string(1, ch));
    c.Ret(ch < 0 ? 0 : static_cast<u64>(ch));
}

void shim_puts(ShimCall &c) {
    const std::string s = c.ArgStrA(0);
    c.proc->ConsoleWrite(s + "\n");
    c.Ret(s.size() + 1);
}

void shim___iob_func(ShimCall &c) { c.Ret(EnsureIOB(c)); }
void shim___acrt_iob_func(ShimCall &c) {
    c.Ret(EnsureIOB(c) + kFileSize * static_cast<u64>(c.Arg(0)));
}

// int __getmainargs(int* argc, char*** argv, char*** envp, int expand, int* newmode)
void shim___getmainargs(ShimCall &c) {
    const u64 pac = c.Arg(0), pav = c.Arg(1), penv = c.Arg(2);

    u64 namebuf = c.proc->heap().Alloc(256, true);
    c.mem->WriteCString(namebuf, c.proc->image().name);

    u64 arr = c.proc->heap().Alloc(32, true);
    c.mem->Write64(arr, namebuf);
    c.mem->Write64(arr + 8, 0);          // argv[] 以 NULL 结尾

    u64 env = c.proc->heap().Alloc(16, true);   // 空环境

    if (pac) c.mem->Write32(pac, 1);
    if (pav) c.mem->Write64(pav, arr);
    if (penv) c.mem->Write64(penv, env);
    c.Ret(0);
}

// _initterm：C++ 全局构造器表。我们不执行它们（会牵出一大堆 guest 代码），
// 直接跳过。对 C 程序没影响。
void shim__initterm(ShimCall &c) { c.Ret(0); }
void shim__initterm_e(ShimCall &c) { c.Ret(0); }

void shim___set_app_type(ShimCall &c) { c.Ret(0); }
void shim__setusermatherr(ShimCall &c) { c.Ret(0); }
void shim___setusermatherr(ShimCall &c) { c.Ret(0); }
void shim___C_specific_handler(ShimCall &c) { c.Ret(0); }
void shim__lock(ShimCall &c) { c.Ret(0); }
void shim__unlock(ShimCall &c) { c.Ret(0); }
void shim__amsg_exit(ShimCall &c) { c.Ret(0); }
void shim__cexit(ShimCall &c) { c.Ret(0); }
void shim__onexit(ShimCall &c) { c.Ret(0); }
void shim_signal(ShimCall &c) { c.Ret(0); }
void shim_localeconv(ShimCall &c) { c.Ret(0); }

// 这几个返回的是「某个全局变量的地址」，必须给来宾地址空间里的位置
u64 StaticSlot(ShimCall &c, int idx, u32 value) {
    static u64 base = 0;
    if (!base) base = c.proc->heap().Alloc(64, true);
    c.mem->Write32(base + 4 * idx, value);
    return base + 4 * idx;
}
// _errno() 返回的是「int 变量的地址」，不是变量本身
void shim__errno(ShimCall &c) { c.Ret(StaticSlot(c, 4, 0)); }
void shim___lc_codepage_func(ShimCall &c) { c.Ret(StaticSlot(c, 0, 1252)); }
void shim____lc_codepage_func(ShimCall &c) { c.Ret(StaticSlot(c, 0, 1252)); }
void shim____mb_cur_max_func(ShimCall &c) { c.Ret(StaticSlot(c, 1, 1)); }


void shim_strerror(ShimCall &c) {
    static u64 buf = 0;
    if (!buf) buf = c.proc->heap().Alloc(64, true);
    c.mem->WriteCString(buf, "error");
    c.Ret(buf);
}

void shim_strncmp(ShimCall &c) {
    const std::string a = c.ArgStrA(0), b = c.ArgStrA(1);
    const usize n = static_cast<usize>(c.Arg(2));
    const int r = a.compare(0, n, b, 0, n);
    c.Ret(static_cast<u64>(r));
}

void shim_wcslen(ShimCall &c) { c.Ret(static_cast<u64>(c.ArgStrW(0).size())); }

void shim_malloc(ShimCall &c) {
    c.Ret(c.proc->heap().Alloc(static_cast<usize>(c.Arg(0)), false));
}

void shim_calloc(ShimCall &c) {
    const usize n = static_cast<usize>(c.Arg(0)) * static_cast<usize>(c.Arg(1));
    c.Ret(c.proc->heap().Alloc(n, true));
}

void shim_free(ShimCall &c) {
    c.proc->heap().Free(c.Arg(0));
    c.Ret(0);
}

void shim_exit(ShimCall &c) { c.Exit(c.Arg(0)); }
void shim_abort(ShimCall &c) { c.Exit(3); }

}  // namespace

void RegisterCrt(ShimTable &t) {
    const char *dlls[] = {"msvcrt.dll", "msvcr40.dll", "crtdll.dll",
                          "vcruntime140.dll", "api-ms-win-crt-stdio-l1-1-0.dll",
                          "api-ms-win-crt-runtime-l1-1-0.dll"};

    for (const char *d : dlls) {
        t.Register(d, "fprintf", shim_fprintf);
        t.Register(d, "vfprintf", shim_vfprintf);
        t.Register(d, "printf", shim_printf);
        t.Register(d, "fwrite", shim_fwrite);
        t.Register(d, "fputc", shim_fputc);
        t.Register(d, "puts", shim_puts);
        t.Register(d, "__iob_func", shim___iob_func);
        t.Register(d, "__acrt_iob_func", shim___acrt_iob_func);
        t.Register(d, "__getmainargs", shim___getmainargs);
        t.Register(d, "_initterm", shim__initterm);
        t.Register(d, "_initterm_e", shim__initterm_e);
        t.Register(d, "__set_app_type", shim___set_app_type);
        t.Register(d, "__setusermatherr", shim___setusermatherr);
        t.Register(d, "___setusermatherr", shim___setusermatherr);
        t.Register(d, "__C_specific_handler", shim___C_specific_handler);
        t.Register(d, "_lock", shim__lock);
        t.Register(d, "_unlock", shim__unlock);
        t.Register(d, "_amsg_exit", shim__amsg_exit);
        t.Register(d, "_cexit", shim__cexit);
        t.Register(d, "_onexit", shim__onexit);
        t.Register(d, "signal", shim_signal);
        t.Register(d, "localeconv", shim_localeconv);
        t.Register(d, "__lc_codepage_func", shim___lc_codepage_func);
        t.Register(d, "___lc_codepage_func", shim____lc_codepage_func);
        t.Register(d, "___mb_cur_max_func", shim____mb_cur_max_func);
        t.Register(d, "_errno", shim__errno);
        t.Register(d, "strerror", shim_strerror);
        t.Register(d, "strncmp", shim_strncmp);
        t.Register(d, "wcslen", shim_wcslen);
        t.Register(d, "malloc", shim_malloc);
        t.Register(d, "calloc", shim_calloc);
        t.Register(d, "free", shim_free);
        t.Register(d, "exit", shim_exit);
        t.Register(d, "abort", shim_abort);
    }
}

}  // namespace hxvm
