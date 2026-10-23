// SPDX-License-Identifier: MIT
// USER32 垫片。
//
// 目标游戏是音游，窗口/消息循环这些对它基本无意义：
// 一律给「看起来正常」的返回值，把真正有价值的调用（比如 MessageBox）转成人能看见的日志。
#include "hxvm/shim.hpp"

#include "hxvm/log.hpp"
#include "hxvm/process.hpp"

namespace hxvm {

void RegisterUser32(ShimTable &t) {
    const char *k = "user32.dll";

    auto ret0 = [](ShimCall &c) { c.Ret(0); };
    auto ret1 = [](ShimCall &c) { c.Ret(1); };

    // 弹窗：直接转成日志，别真的去弹 iOS 弹窗阻塞模拟
    t.Register(k, "MessageBoxA", [](ShimCall &c) {
        HX_LOGI("guest", "MessageBoxA: %s", c.ArgStrA(1).c_str());
        c.Ret(1);   // IDOK
    });
    t.Register(k, "MessageBoxW", [](ShimCall &c) {
        HX_LOGI("guest", "MessageBoxW 被调用");
        c.Ret(1);
    });

    // 输入状态：这是后面做触控注入的落点（阶段五）。
    // 先老老实实返回「没有按键」，等真要做输入时改成读一块共享内存。
    t.Register(k, "GetAsyncKeyState", ret0);
    t.Register(k, "GetKeyState", ret0);
    t.Register(k, "GetKeyboardState", [](ShimCall &c) {
        if (c.Arg(0)) c.mem->Fill(c.Arg(0), 0, 256);
        c.Ret(1);
    });
    t.Register(k, "GetCursorPos", [](ShimCall &c) {
        if (c.Arg(0)) { c.mem->Write32(c.Arg(0), 0); c.mem->Write32(c.Arg(0) + 4, 0); }
        c.Ret(1);
    });
    t.Register(k, "SetCursorPos", ret1);
    t.Register(k, "ShowCursor", ret0);
    t.Register(k, "GetMessagePos", ret0);
    t.Register(k, "MapVirtualKeyA", ret0);
    t.Register(k, "MapVirtualKeyW", ret0);

    // 窗口：模拟里没有真窗口，给句柄但不做任何事
    t.Register(k, "RegisterClassA", ret1);
    t.Register(k, "RegisterClassW", ret1);
    t.Register(k, "RegisterClassExA", ret1);
    t.Register(k, "RegisterClassExW", ret1);
    t.Register(k, "CreateWindowExA", [](ShimCall &c) { c.Ret(0x1000); });
    t.Register(k, "CreateWindowExW", [](ShimCall &c) { c.Ret(0x1000); });
    t.Register(k, "GetDesktopWindow", [](ShimCall &c) { c.Ret(0x1000); });
    t.Register(k, "GetActiveWindow", [](ShimCall &c) { c.Ret(0x1000); });
    t.Register(k, "GetForegroundWindow", [](ShimCall &c) { c.Ret(0x1000); });
    t.Register(k, "ShowWindow", ret1);
    t.Register(k, "UpdateWindow", ret1);
    t.Register(k, "DestroyWindow", ret1);
    t.Register(k, "SetWindowTextA", ret1);
    t.Register(k, "SetWindowTextW", ret1);
    t.Register(k, "GetWindowTextA", ret0);
    t.Register(k, "GetWindowTextW", ret0);
    t.Register(k, "InvalidateRect", ret1);
    t.Register(k, "SetFocus", ret1);
    t.Register(k, "ReleaseCapture", ret1);
    t.Register(k, "SetWindowLongA", ret0);
    t.Register(k, "SetWindowLongW", ret0);
    t.Register(k, "GetWindowLongA", ret0);
    t.Register(k, "GetWindowLongW", ret0);
    t.Register(k, "AdjustWindowRect", ret1);
    t.Register(k, "MoveWindow", ret1);
    t.Register(k, "SetWindowPos", ret1);

    // 消息循环：一律「没有消息」，让游戏别在消息泵里打转
    t.Register(k, "PeekMessageA", ret0);
    t.Register(k, "PeekMessageW", ret0);
    t.Register(k, "GetMessageA", ret0);
    t.Register(k, "GetMessageW", ret0);
    t.Register(k, "TranslateMessage", ret0);
    t.Register(k, "DispatchMessageA", ret0);
    t.Register(k, "DispatchMessageW", ret0);
    t.Register(k, "DefWindowProcA", ret0);
    t.Register(k, "DefWindowProcW", ret0);
    t.Register(k, "PostMessageA", ret1);
    t.Register(k, "PostMessageW", ret1);
    t.Register(k, "SendMessageA", ret0);
    t.Register(k, "SendMessageW", ret0);
    t.Register(k, "PostQuitMessage", ret0);
    t.Register(k, "WaitMessage", ret1);
    t.Register(k, "MsgWaitForMultipleObjects", ret0);

    // 度量 / 矩形：给 1080p 级别的值，避免除零
    t.Register(k, "GetSystemMetrics", [](ShimCall &c) {
        const i32 idx = static_cast<i32>(static_cast<u32>(c.Arg(0)));
        if (idx == 0 || idx == 78) c.Ret(1920);   // SM_CXSCREEN / CXVIRTUALSCREEN
        if (idx == 1 || idx == 79) c.Ret(1080);
        c.Ret(96);
    });
    t.Register(k, "GetClientRect", [](ShimCall &c) {
        const u64 r = c.Arg(1);
        c.mem->Write32(r + 0, 0);
        c.mem->Write32(r + 4, 0);
        c.mem->Write32(r + 8, 1920);
        c.mem->Write32(r + 12, 1080);
        c.Ret(1);
    });
    t.Register(k, "GetWindowRect", [](ShimCall &c) {
        const u64 r = c.Arg(1);
        c.mem->Write32(r + 0, 0);
        c.mem->Write32(r + 4, 0);
        c.mem->Write32(r + 8, 1920);
        c.mem->Write32(r + 12, 1080);
        c.Ret(1);
    });

    t.Register(k, "GetDC", [](ShimCall &c) { c.Ret(0x2000); });
    t.Register(k, "ReleaseDC", ret1);
    t.Register(k, "LoadCursorA", ret0);
    t.Register(k, "LoadCursorW", ret0);
    t.Register(k, "LoadIconA", ret0);
    t.Register(k, "LoadIconW", ret0);
    t.Register(k, "SetTimer", ret1);
    t.Register(k, "KillTimer", ret1);
    t.Register(k, "RegisterRawInputDevices", ret1);
    t.Register(k, "EnumDisplaySettingsA", ret1);
    t.Register(k, "EnumDisplaySettingsW", ret1);
    t.Register(k, "ChangeDisplaySettingsA", ret0);
    t.Register(k, "ChangeDisplaySettingsW", ret0);
    t.Register(k, "wsprintfA", ret0);
    t.Register(k, "wsprintfW", ret0);
    t.Register(k, "CharUpperA", [](ShimCall &c) { c.Ret(c.Arg(0)); });
    t.Register(k, "CharLowerA", [](ShimCall &c) { c.Ret(c.Arg(0)); });
    t.Register(k, "CharUpperW", [](ShimCall &c) { c.Ret(c.Arg(0)); });
    t.Register(k, "CharLowerW", [](ShimCall &c) { c.Ret(c.Arg(0)); });
    t.Register(k, "IsWindowVisible", ret1);
    t.Register(k, "ClientToScreen", ret1);
    t.Register(k, "ScreenToClient", ret1);
}

}  // namespace hxvm
