// SPDX-License-Identifier: MIT
// ADVAPI32 垫片：内存里伪造一个注册表。
//
// 目的不是「真的有注册表」，而是让程序读常见键时拿到合理值，
// 不至于因为拿到空值而走进奇怪的分支。
#include "hxvm/shim.hpp"

#include <string>

#include "hxvm/log.hpp"
#include "hxvm/process.hpp"

namespace hxvm {
namespace {

std::string Lower(const std::string &s) {
    std::string o = s;
    for (char &c : o) if (c >= 'A' && c <= 'Z') c += 32;
    return o;
}

// 把 (HKEY, 子键) 拼成完整路径
//
// 注意 HKEY_* 常量在 Windows SDK 里是这样定义的：
//     #define HKEY_LOCAL_MACHINE ((HKEY)(ULONG_PTR)((LONG)0x80000002))
// 也就是先当 32 位有符号数、再做符号扩展 —— 64 位下实际值是
// 0xFFFFFFFF80000002，不是 0x80000002。只比较低 32 位才能稳定匹配。
std::string BuildPath(ShimCall &c, int key_idx, int sub_idx) {
    const u64 hkey = c.Arg(key_idx);
    std::string base;
    switch (static_cast<u32>(hkey & 0xFFFFFFFFull)) {
        case 0x80000000u: base = "HKEY_CLASSES_ROOT"; break;
        case 0x80000001u: base = "HKEY_CURRENT_USER"; break;
        case 0x80000002u: base = "HKEY_LOCAL_MACHINE"; break;
        case 0x80000003u: base = "HKEY_USERS"; break;
        case 0x80000005u: base = "HKEY_CURRENT_CONFIG"; break;
        default: {
            // 已经打开过的键句柄：向 Registry 要它的完整路径
            const std::string opened = c.proc->reg().KeyPath(hkey);
            if (!opened.empty()) return opened;
            base = "HKEY_CURRENT_USER";
            break;
        }
    }
    const std::string sub = c.ArgStrA(sub_idx);
    if (sub.empty()) return base;
    return base + "\\" + sub;
}

u64 OpenCommon(ShimCall &c, const std::string &full, bool create) {
    const u64 key = c.proc->reg().OpenKey(full, create);
    HX_LOGT("reg", "OpenKey(\"%s\", create=%d) -> %llu", full.c_str(), (int)create,
            (unsigned long long)key);
    if (!key) {
        c.proc->SetLastError(2);   // ERROR_FILE_NOT_FOUND
        return 2;                  // 必须返回错误码，返回 0 等于告诉调用方成功了
    }
    Handle h;
    h.kind = Handle::kKey;
    h.name = full;
    h.guest_addr = key;
    // 注册表句柄直接用 Registry 返回的句柄值，省一层映射
    c.proc->SetLastError(0);
    h.host = reinterpret_cast<void *>(static_cast<uintptr_t>(key));
    c.proc->NewHandle(h);
    return key;
}

// RegOpenKeyExA(hKey, lpSubKey, ulOptions, samDesired, phkResult)
// 前 4 个参数在寄存器里，第 5 个 phkResult 在栈上（RSP+0x28）。
// 返回值是 LONG 错误码（0 = 成功），句柄必须写进 phkResult 指向的位置。
void shim_RegOpenKeyExA(ShimCall &c) {
    const u64 key = OpenCommon(c, BuildPath(c, 0, 1), false);
    if (!key) { c.Ret(2); return; }          // ERROR_FILE_NOT_FOUND
    const u64 phk = c.StackArg(0);
    if (phk) c.mem->Write64(phk, key);
    c.Ret(0);
}

void shim_RegOpenKeyExW(ShimCall &c) {
    const u64 key = OpenCommon(c, BuildPath(c, 0, 1), false);
    if (!key) { c.Ret(2); return; }
    const u64 phk = c.StackArg(0);
    if (phk) c.mem->Write64(phk, key);
    c.Ret(0);
}

// RegCreateKeyExA(hKey, lpSubKey, Reserved, lpClass, dwOptions, samDesired,
//                 lpSecurityAttributes, phkResult, lpdwDisposition)
void shim_RegCreateKeyExA(ShimCall &c) {
    const u64 key = OpenCommon(c, BuildPath(c, 0, 1), true);
    if (!key) { c.Ret(2); return; }
    const u64 phk = c.StackArg(3);        // 第 8 个参数
    if (phk) c.mem->Write64(phk, key);
    const u64 disp = c.StackArg(4);       // 第 9 个参数
    if (disp) c.mem->Write32(disp, 1);    // REG_CREATED_NEW_KEY
    c.Ret(0);
}

void shim_RegCreateKeyExW(ShimCall &c) { shim_RegCreateKeyExA(c); }

void shim_RegCloseKey(ShimCall &c) {
    c.proc->reg().CloseKey(c.Arg(0));
    c.proc->CloseHandle(c.Arg(0));
    c.Ret(0);   // ERROR_SUCCESS
}

// 返回 Windows 意义的错误码（不是 bool）
// RegQueryValueExA(hKey, lpValueName, lpReserved, lpType, lpData, lpcbData)
// lpData/lpcbData 是第 5、6 个参数，落在栈上（RSP+0x28、RSP+0x30）
void shim_RegQueryValueExA(ShimCall &c) {
    const u64 name_ptr = c.Arg(1);
    std::string vname;
    if (name_ptr) vname = c.mem->ReadCString(name_ptr);

    u32 type = 0;
    std::vector<u8> data;
    if (!c.proc->reg().QueryValue(c.Arg(0), vname, type, data)) {
        c.Ret(2);   // ERROR_FILE_NOT_FOUND
        return;
    }

    // lpType（第 4 个参数）
    if (c.Arg(3)) c.mem->Write32(c.Arg(3), type);

    const u64 lp_data = c.Arg(4);
    const u64 lp_cb = c.Arg(5);

    // 字符串类型要补终止符，否则调用方一眼望不到头
    std::vector<u8> with_nul = data;
    if (type == kRegSz || type == kRegExpandSz || type == kRegMultiSz) {
        with_nul.push_back(0);
        with_nul.push_back(0);   // 给 UTF-16 转换留余量
    }

    if (lp_cb) {
        const u32 buf = c.mem->Read32(lp_cb);
        if (lp_data && buf > 0) {
            const usize copy = std::min<usize>(buf, with_nul.size());
            if (copy) c.mem->Write(lp_data, with_nul.data(), copy);
        }
        c.mem->Write32(lp_cb, static_cast<u32>(with_nul.size()));
    }
    c.Ret(0);   // ERROR_SUCCESS
}

void shim_RegQueryValueExW(ShimCall &c) { shim_RegQueryValueExA(c); }

void shim_RegSetValueExA(ShimCall &c) {
    const u32 size = c.mem->Read32(c.Arg(5));
    std::vector<u8> data(size);
    if (size) c.mem->Read(c.Arg(4), data.data(), size);
    c.proc->reg().SetValue(c.Arg(0), c.ArgStrA(1), c.mem->Read32(c.Arg(2)),
                           data.data(), size);
    c.Ret(0);
}

void shim_RegSetValueExW(ShimCall &c) { shim_RegSetValueExA(c); }

void shim_RegEnumKeyExA(ShimCall &c) { c.Ret(259); }   // ERROR_NO_MORE_ITEMS
void shim_RegEnumKeyExW(ShimCall &c) { c.Ret(259); }
void shim_RegEnumValueA(ShimCall &c) { c.Ret(259); }
void shim_RegEnumValueW(ShimCall &c) { c.Ret(259); }
void shim_RegDeleteKeyA(ShimCall &c) { c.Ret(0); }
void shim_RegDeleteValueA(ShimCall &c) { c.Ret(0); }
void shim_RegQueryInfoKeyA(ShimCall &c) { c.Ret(0); }
void shim_RegGetValueA(ShimCall &c) { shim_RegQueryValueExA(c); }
void shim_RegGetValueW(ShimCall &c) { shim_RegQueryValueExA(c); }

void shim_GetUserNameA(ShimCall &c) {
    const std::string name = "Player";
    if (c.Arg(0)) c.mem->WriteCString(c.Arg(0), name);
    c.Ret(1);
}

void shim_GetUserNameW(ShimCall &c) { c.Ret(1); }
void shim_OpenProcessToken(ShimCall &c) { c.Ret(1); }
void shim_OpenThreadToken(ShimCall &c) { c.Ret(0); }
void shim_GetTokenInformation(ShimCall &c) { c.Ret(0); }
void shim_InitializeSecurityDescriptor(ShimCall &c) { c.Ret(1); }
void shim_SetSecurityDescriptorDacl(ShimCall &c) { c.Ret(1); }
void shim_CryptAcquireContextA(ShimCall &c) { c.Ret(1); }
void shim_CryptReleaseContext(ShimCall &c) { c.Ret(1); }
void shim_CryptGenRandom(ShimCall &c) { c.Ret(1); }
void shim_LookupPrivilegeValueA(ShimCall &c) { c.Ret(1); }
void shim_AdjustTokenPrivileges(ShimCall &c) { c.Ret(1); }
void shim_RegFlushKey(ShimCall &c) { c.Ret(0); }

}  // namespace

void RegisterAdvapi32(ShimTable &t) {
    const char *k = "advapi32.dll";
    t.Register(k, "RegOpenKeyExA", shim_RegOpenKeyExA);
    t.Register(k, "RegOpenKeyExW", shim_RegOpenKeyExW);
    t.Register(k, "RegCreateKeyExA", shim_RegCreateKeyExA);
    t.Register(k, "RegCreateKeyExW", shim_RegCreateKeyExW);
    t.Register(k, "RegCloseKey", shim_RegCloseKey);
    t.Register(k, "RegQueryValueExA", shim_RegQueryValueExA);
    t.Register(k, "RegQueryValueExW", shim_RegQueryValueExW);
    t.Register(k, "RegSetValueExA", shim_RegSetValueExA);
    t.Register(k, "RegSetValueExW", shim_RegSetValueExW);
    t.Register(k, "RegEnumKeyExA", shim_RegEnumKeyExA);
    t.Register(k, "RegEnumKeyExW", shim_RegEnumKeyExW);
    t.Register(k, "RegEnumValueA", shim_RegEnumValueA);
    t.Register(k, "RegEnumValueW", shim_RegEnumValueW);
    t.Register(k, "RegDeleteKeyA", shim_RegDeleteKeyA);
    t.Register(k, "RegDeleteValueA", shim_RegDeleteValueA);
    t.Register(k, "RegQueryInfoKeyA", shim_RegQueryInfoKeyA);
    t.Register(k, "RegGetValueA", shim_RegGetValueA);
    t.Register(k, "RegGetValueW", shim_RegGetValueW);
    t.Register(k, "RegFlushKey", shim_RegFlushKey);
    t.Register(k, "GetUserNameA", shim_GetUserNameA);
    t.Register(k, "GetUserNameW", shim_GetUserNameW);
    t.Register(k, "OpenProcessToken", shim_OpenProcessToken);
    t.Register(k, "OpenThreadToken", shim_OpenThreadToken);
    t.Register(k, "GetTokenInformation", shim_GetTokenInformation);
    t.Register(k, "InitializeSecurityDescriptor", shim_InitializeSecurityDescriptor);
    t.Register(k, "SetSecurityDescriptorDacl", shim_SetSecurityDescriptorDacl);
    t.Register(k, "CryptAcquireContextA", shim_CryptAcquireContextA);
    t.Register(k, "CryptReleaseContext", shim_CryptReleaseContext);
    t.Register(k, "CryptGenRandom", shim_CryptGenRandom);
    t.Register(k, "LookupPrivilegeValueA", shim_LookupPrivilegeValueA);
    t.Register(k, "AdjustTokenPrivileges", shim_AdjustTokenPrivileges);
}

}  // namespace hxvm
