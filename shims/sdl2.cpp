// SPDX-License-Identifier: MIT
// SDL2 垫片。
//
// 这是渲染的「正确拦截边界」：OpenFL/lime 在 Windows 上是 SDL2 + OpenGL(经 ANGLE 转 D3D)，
// 所以目标 EXE 几乎一定会 import SDL2.dll。把 SDL2 整个换到 iOS 官方 SDL2（Metal 后端）
// 就能拿到硬件加速 —— 比原方案里「抓帧缓冲再贴到 CAMetalLayer」干净得多，也快得多。
//
// 当前状态：先注册接口，让游戏能初始化并通过；真正的绘制命令转发是下一步。
// 抓屏（FrameBuffer 拦截）只作为这些接口都接不上时的兜底。
#include "hxvm/shim.hpp"

#include <map>
#include <string>

#include "hxvm/log.hpp"
#include "hxvm/process.hpp"
#include "shims/media_backend.hpp"

namespace hxvm {
namespace {

// 模拟的 SDL 对象
struct SdlObject {
    enum Kind { kWindow, kRenderer, kTexture, kSurface, kContext } kind;
    int w = 1280, h = 720;
};
std::map<u64, SdlObject> g_objs;
u64 g_next = 1;

u64 New(SdlObject o) {
    const u64 id = g_next++;
    g_objs[id] = o;
    return id;
}

// SDL_Init(flags)
void shim_SDL_Init(ShimCall &c) {
    const u32 flags = static_cast<u32>(c.Arg(0));
    HX_LOGI("sdl2", "SDL_Init(flags=%#x)", flags);
    c.Ret(0);   // 成功
}
void shim_SDL_InitSubSystem(ShimCall &c) { c.Ret(0); }
void shim_SDL_Quit(ShimCall &c) { c.Ret(0); }
void shim_SDL_QuitSubSystem(ShimCall &c) { c.Ret(0); }
void shim_SDL_WasInit(ShimCall &c) { c.Ret(0); }

// 窗口 / 渲染器
void shim_SDL_CreateWindow(ShimCall &c) {
    const int w = static_cast<int>(c.Arg(2));
    const int h = static_cast<int>(c.Arg(3));
    HX_LOGI("sdl2", "SDL_CreateWindow(%s, %dx%d)", c.ArgStrA(0).c_str(), w, h);
    const RenderBackend &r = GetRenderBackend();
    if (r.create_window) r.create_window(c.ArgStrA(0), w, h);
    c.Ret(New({SdlObject::kWindow, w, h}));
}

void shim_SDL_CreateRenderer(ShimCall &c) {
    c.Ret(New({SdlObject::kRenderer}));
}

void shim_SDL_CreateWindowAndRenderer(ShimCall &c) {
    const int w = static_cast<int>(c.Arg(2));
    const int h = static_cast<int>(c.Arg(3));
    c.mem->Write64(c.Arg(0), New({SdlObject::kWindow, w, h}));
    c.mem->Write64(c.Arg(1), New({SdlObject::kRenderer}));
    c.Ret(0);
}

void shim_SDL_DestroyWindow(ShimCall &c) {
    const RenderBackend &r = GetRenderBackend();
    if (r.destroy_window) r.destroy_window();
    g_objs.erase(c.Arg(0));
    c.Ret(0);
}
void shim_SDL_DestroyRenderer(ShimCall &c) { g_objs.erase(c.Arg(0)); c.Ret(0); }
void shim_SDL_SetWindowTitle(ShimCall &c) { c.Ret(0); }
void shim_SDL_SetWindowSize(ShimCall &c) { c.Ret(0); }
void shim_SDL_GetWindowSize(ShimCall &c) {
    if (c.Arg(1)) c.mem->Write32(c.Arg(1), 1280);
    if (c.Arg(2)) c.mem->Write32(c.Arg(2), 720);
    c.Ret(0);
}
void shim_SDL_ShowWindow(ShimCall &c) { c.Ret(0); }
void shim_SDL_HideWindow(ShimCall &c) { c.Ret(0); }
void shim_SDL_RaiseWindow(ShimCall &c) { c.Ret(0); }
void shim_SDL_MaximizeWindow(ShimCall &c) { c.Ret(0); }
void shim_SDL_SetWindowFullscreen(ShimCall &c) { c.Ret(0); }
void shim_SDL_SetWindowGrab(ShimCall &c) { c.Ret(0); }
void shim_SDL_GetWindowID(ShimCall &c) { c.Ret(1); }
void shim_SDL_GetWindowFlags(ShimCall &c) { c.Ret(0); }

// 绘制
void shim_SDL_SetRenderDrawColor(ShimCall &c) { c.Ret(0); }
void shim_SDL_RenderClear(ShimCall &c) {
    const RenderBackend &r = GetRenderBackend();
    if (r.clear) r.clear(0, 0, 0, 1);
    c.Ret(0);
}
void shim_SDL_RenderPresent(ShimCall &c) {
    const RenderBackend &r = GetRenderBackend();
    if (r.present) r.present();
    c.Ret(0);
}
void shim_SDL_RenderCopy(ShimCall &c) { c.Ret(0); }
void shim_SDL_RenderCopyEx(ShimCall &c) { c.Ret(0); }
void shim_SDL_RenderFillRect(ShimCall &c) { c.Ret(0); }
void shim_SDL_RenderDrawLine(ShimCall &c) { c.Ret(0); }
void shim_SDL_SetRenderTarget(ShimCall &c) { c.Ret(0); }
void shim_SDL_GetRendererOutputSize(ShimCall &c) {
    if (c.Arg(1)) c.mem->Write32(c.Arg(1), 1280);
    if (c.Arg(2)) c.mem->Write32(c.Arg(2), 720);
    c.Ret(0);
}
void shim_SDL_GetRendererInfo(ShimCall &c) { c.Ret(0); }
void shim_SDL_CreateTexture(ShimCall &c) { c.Ret(New({SdlObject::kTexture})); }
void shim_SDL_CreateTextureFromSurface(ShimCall &c) { c.Ret(New({SdlObject::kTexture})); }
void shim_SDL_DestroyTexture(ShimCall &c) { g_objs.erase(c.Arg(0)); c.Ret(0); }
void shim_SDL_SetTextureBlendMode(ShimCall &c) { c.Ret(0); }
void shim_SDL_SetTextureAlphaMod(ShimCall &c) { c.Ret(0); }
void shim_SDL_SetTextureColorMod(ShimCall &c) { c.Ret(0); }
void shim_SDL_QueryTexture(ShimCall &c) { c.Ret(0); }
void shim_SDL_LockTexture(ShimCall &c) { c.Ret(0); }
void shim_SDL_UnlockTexture(ShimCall &c) { c.Ret(0); }
void shim_SDL_UpdateTexture(ShimCall &c) { c.Ret(0); }
void shim_SDL_SetRenderDrawBlendMode(ShimCall &c) { c.Ret(0); }

// OpenGL（ANGLE）上下文
void shim_SDL_GL_SetAttribute(ShimCall &c) { c.Ret(0); }
void shim_SDL_GL_GetAttribute(ShimCall &c) {
    if (c.Arg(1)) c.mem->Write32(c.Arg(1), 8);
    c.Ret(0);
}
void shim_SDL_GL_CreateContext(ShimCall &c) { c.Ret(New({SdlObject::kContext})); }
void shim_SDL_GL_MakeCurrent(ShimCall &c) { c.Ret(0); }
void shim_SDL_GL_DeleteContext(ShimCall &c) { c.Ret(0); }
void shim_SDL_GL_SetSwapInterval(ShimCall &c) { c.Ret(0); }
void shim_SDL_GL_GetSwapInterval(ShimCall &c) { c.Ret(0); }
void shim_SDL_GL_SwapWindow(ShimCall &c) {
    const RenderBackend &r = GetRenderBackend();
    if (r.present) r.present();
    c.Ret(0);
}
void shim_SDL_GL_GetProcAddress(ShimCall &c) { c.Ret(0); }
void shim_SDL_GL_ExtensionSupported(ShimCall &c) { c.Ret(1); }
void shim_SDL_GL_LoadLibrary(ShimCall &c) { c.Ret(0); }
void shim_SDL_GL_GetDrawableSize(ShimCall &c) {
    if (c.Arg(1)) c.mem->Write32(c.Arg(1), 1280);
    if (c.Arg(2)) c.mem->Write32(c.Arg(2), 720);
    c.Ret(0);
}

// 事件：一律「没有事件」
void shim_SDL_PollEvent(ShimCall &c) { c.Ret(0); }
void shim_SDL_WaitEvent(ShimCall &c) { c.Ret(0); }
void shim_SDL_WaitEventTimeout(ShimCall &c) { c.Ret(0); }
void shim_SDL_PushEvent(ShimCall &c) { c.Ret(1); }
void shim_SDL_GetEventState(ShimCall &c) { c.Ret(1); }
void shim_SDL_FlushEvents(ShimCall &c) { c.Ret(0); }

// 时间
void shim_SDL_GetTicks(ShimCall &c) { c.Ret(c.proc->TickCount()); }
void shim_SDL_GetTicks64(ShimCall &c) { c.Ret(c.proc->TickCount()); }
void shim_SDL_GetPerformanceCounter(ShimCall &c) {
    c.mem->Write64(c.Arg(0), c.proc->Qpc());
}
void shim_SDL_GetPerformanceFrequency(ShimCall &c) {
    c.mem->Write64(c.Arg(0), c.proc->QpcFrequency());
}
void shim_SDL_Delay(ShimCall &c) { c.Ret(0); }

// 显示
void shim_SDL_GetDesktopDisplayMode(ShimCall &c) { c.Ret(0); }
void shim_SDL_GetCurrentDisplayMode(ShimCall &c) { c.Ret(0); }
void shim_SDL_GetDisplayMode(ShimCall &c) { c.Ret(0); }
void shim_SDL_GetNumVideoDisplays(ShimCall &c) { c.Ret(1); }
void shim_SDL_GetNumDisplayModes(ShimCall &c) { c.Ret(1); }
void shim_SDL_GetDisplayBounds(ShimCall &c) { c.Ret(0); }
void shim_SDL_GetDisplayDPI(ShimCall &c) { c.Ret(0); }

// 音频
void shim_SDL_OpenAudio(ShimCall &c) { c.Ret(0); }
void shim_SDL_OpenAudioDevice(ShimCall &c) { c.Ret(2); }
void shim_SDL_CloseAudio(ShimCall &c) { c.Ret(0); }
void shim_SDL_CloseAudioDevice(ShimCall &c) { c.Ret(0); }
void shim_SDL_PauseAudio(ShimCall &c) { c.Ret(0); }
void shim_SDL_PauseAudioDevice(ShimCall &c) { c.Ret(0); }
void shim_SDL_QueueAudio(ShimCall &c) { c.Ret(0); }
void shim_SDL_GetAudioStatus(ShimCall &c) { c.Ret(0); }
void shim_SDL_MixAudio(ShimCall &c) { c.Ret(0); }
void shim_SDL_GetAudioDeviceStatus(ShimCall &c) { c.Ret(0); }
void shim_SDL_AudioInit(ShimCall &c) { c.Ret(0); }
void shim_SDL_AudioQuit(ShimCall &c) { c.Ret(0); }
void shim_SDL_GetNumAudioDevices(ShimCall &c) { c.Ret(1); }
void shim_SDL_GetAudioDeviceName(ShimCall &c) { c.Ret(0); }

// 输入
void shim_SDL_GetKeyboardState(ShimCall &c) { c.Ret(0); }
void shim_SDL_ShowCursor(ShimCall &c) { c.Ret(0); }
void shim_SDL_StartTextInput(ShimCall &c) { c.Ret(0); }
void shim_SDL_StopTextInput(ShimCall &c) { c.Ret(0); }
void shim_SDL_NumJoysticks(ShimCall &c) { c.Ret(0); }
void shim_SDL_JoystickOpen(ShimCall &c) { c.Ret(0); }
void shim_SDL_GetMouseState(ShimCall &c) { c.Ret(0); }
void shim_SDL_GetRelativeMouseState(ShimCall &c) { c.Ret(0); }

// 其它
void shim_SDL_GetError(ShimCall &c) { c.Ret(0); }
void shim_SDL_SetError(ShimCall &c) { c.Ret(0); }
void shim_SDL_GetHint(ShimCall &c) { c.Ret(0); }
void shim_SDL_SetHint(ShimCall &c) { c.Ret(0); }
void shim_SDL_GetBasePath(ShimCall &c) { c.Ret(0); }
void shim_SDL_GetPrefPath(ShimCall &c) { c.Ret(0); }
void shim_SDL_GetPowerInfo(ShimCall &c) { c.Ret(0); }
void shim_SDL_GetCPUCount(ShimCall &c) { c.Ret(4); }
void shim_SDL_GetSystemRAM(ShimCall &c) { c.Ret(8192); }
void shim_SDL_GetPlatform(ShimCall &c) { c.Ret(0); }
void shim_SDL_GetVersion(ShimCall &c) { c.Ret(0); }
void shim_SDL_Log(ShimCall &c) { c.Ret(0); }

// 文件读写：OpenFL 的 Assets 从这里读，路径必须过文件系统解析
void shim_SDL_RWFromFile(ShimCall &c) {
    const std::string raw = c.ArgStrA(0);
    const std::string real = c.proc->fs().Resolve(raw);
    if (real.empty()) {
        HX_LOGW("sdl2", "RWFromFile 解析失败：%s", raw.c_str());
        c.Ret(0);
        return;
    }
    FILE *f = fopen(real.c_str(), c.ArgStrA(1).empty() ? "rb" : "rb");
    if (!f) { c.Ret(0); return; }
    Handle h;
    h.kind = Handle::kFile;
    h.host = f;
    h.name = raw;
    c.Ret(c.proc->NewHandle(h));
}
void shim_SDL_RWFromMem(ShimCall &c) { c.Ret(0); }
void shim_SDL_RWread(ShimCall &c) { c.Ret(0); }
void shim_SDL_RWwrite(ShimCall &c) { c.Ret(0); }
void shim_SDL_RWseek(ShimCall &c) { c.Ret(0); }
void shim_SDL_RWtell(ShimCall &c) { c.Ret(0); }
void shim_SDL_RWsize(ShimCall &c) { c.Ret(0); }
void shim_SDL_RWclose(ShimCall &c) { c.Ret(0); }

void shim_SDL_CreateRGBSurface(ShimCall &c) { c.Ret(New({SdlObject::kSurface})); }
void shim_SDL_FreeSurface(ShimCall &c) { c.Ret(0); }
void shim_SDL_FillRect(ShimCall &c) { c.Ret(0); }
void shim_SDL_UpperBlit(ShimCall &c) { c.Ret(0); }
void shim_SDL_ConvertSurface(ShimCall &c) { c.Ret(0); }
void shim_SDL_LoadBMP_RW(ShimCall &c) { c.Ret(0); }
void shim_SDL_LoadWAV_RW(ShimCall &c) { c.Ret(0); }
void shim_SDL_FreeWAV(ShimCall &c) { c.Ret(0); }

}  // namespace

void RegisterSdl2(ShimTable &t) {
    const char *k = "SDL2.dll";
    const char *k2 = "SDL.dll";

    for (const char *dll : {k, k2}) {
        t.Register(dll, "SDL_Init", shim_SDL_Init);
        t.Register(dll, "SDL_InitSubSystem", shim_SDL_InitSubSystem);
        t.Register(dll, "SDL_Quit", shim_SDL_Quit);
        t.Register(dll, "SDL_QuitSubSystem", shim_SDL_QuitSubSystem);
        t.Register(dll, "SDL_WasInit", shim_SDL_WasInit);

        t.Register(dll, "SDL_CreateWindow", shim_SDL_CreateWindow);
        t.Register(dll, "SDL_CreateRenderer", shim_SDL_CreateRenderer);
        t.Register(dll, "SDL_CreateWindowAndRenderer", shim_SDL_CreateWindowAndRenderer);
        t.Register(dll, "SDL_DestroyWindow", shim_SDL_DestroyWindow);
        t.Register(dll, "SDL_DestroyRenderer", shim_SDL_DestroyRenderer);
        t.Register(dll, "SDL_SetWindowTitle", shim_SDL_SetWindowTitle);
        t.Register(dll, "SDL_SetWindowSize", shim_SDL_SetWindowSize);
        t.Register(dll, "SDL_GetWindowSize", shim_SDL_GetWindowSize);
        t.Register(dll, "SDL_ShowWindow", shim_SDL_ShowWindow);
        t.Register(dll, "SDL_HideWindow", shim_SDL_HideWindow);
        t.Register(dll, "SDL_RaiseWindow", shim_SDL_RaiseWindow);
        t.Register(dll, "SDL_MaximizeWindow", shim_SDL_MaximizeWindow);
        t.Register(dll, "SDL_SetWindowFullscreen", shim_SDL_SetWindowFullscreen);
        t.Register(dll, "SDL_SetWindowGrab", shim_SDL_SetWindowGrab);
        t.Register(dll, "SDL_GetWindowID", shim_SDL_GetWindowID);
        t.Register(dll, "SDL_GetWindowFlags", shim_SDL_GetWindowFlags);

        t.Register(dll, "SDL_SetRenderDrawColor", shim_SDL_SetRenderDrawColor);
        t.Register(dll, "SDL_RenderClear", shim_SDL_RenderClear);
        t.Register(dll, "SDL_RenderPresent", shim_SDL_RenderPresent);
        t.Register(dll, "SDL_RenderCopy", shim_SDL_RenderCopy);
        t.Register(dll, "SDL_RenderCopyEx", shim_SDL_RenderCopyEx);
        t.Register(dll, "SDL_RenderFillRect", shim_SDL_RenderFillRect);
        t.Register(dll, "SDL_RenderDrawLine", shim_SDL_RenderDrawLine);
        t.Register(dll, "SDL_SetRenderTarget", shim_SDL_SetRenderTarget);
        t.Register(dll, "SDL_SetRenderDrawBlendMode", shim_SDL_SetRenderDrawBlendMode);
        t.Register(dll, "SDL_GetRendererOutputSize", shim_SDL_GetRendererOutputSize);
        t.Register(dll, "SDL_GetRendererInfo", shim_SDL_GetRendererInfo);
        t.Register(dll, "SDL_CreateTexture", shim_SDL_CreateTexture);
        t.Register(dll, "SDL_CreateTextureFromSurface", shim_SDL_CreateTextureFromSurface);
        t.Register(dll, "SDL_DestroyTexture", shim_SDL_DestroyTexture);
        t.Register(dll, "SDL_SetTextureBlendMode", shim_SDL_SetTextureBlendMode);
        t.Register(dll, "SDL_SetTextureAlphaMod", shim_SDL_SetTextureAlphaMod);
        t.Register(dll, "SDL_SetTextureColorMod", shim_SDL_SetTextureColorMod);
        t.Register(dll, "SDL_QueryTexture", shim_SDL_QueryTexture);
        t.Register(dll, "SDL_LockTexture", shim_SDL_LockTexture);
        t.Register(dll, "SDL_UnlockTexture", shim_SDL_UnlockTexture);
        t.Register(dll, "SDL_UpdateTexture", shim_SDL_UpdateTexture);

        t.Register(dll, "SDL_GL_SetAttribute", shim_SDL_GL_SetAttribute);
        t.Register(dll, "SDL_GL_GetAttribute", shim_SDL_GL_GetAttribute);
        t.Register(dll, "SDL_GL_CreateContext", shim_SDL_GL_CreateContext);
        t.Register(dll, "SDL_GL_MakeCurrent", shim_SDL_GL_MakeCurrent);
        t.Register(dll, "SDL_GL_DeleteContext", shim_SDL_GL_DeleteContext);
        t.Register(dll, "SDL_GL_SetSwapInterval", shim_SDL_GL_SetSwapInterval);
        t.Register(dll, "SDL_GL_GetSwapInterval", shim_SDL_GL_GetSwapInterval);
        t.Register(dll, "SDL_GL_SwapWindow", shim_SDL_GL_SwapWindow);
        t.Register(dll, "SDL_GL_GetProcAddress", shim_SDL_GL_GetProcAddress);
        t.Register(dll, "SDL_GL_ExtensionSupported", shim_SDL_GL_ExtensionSupported);
        t.Register(dll, "SDL_GL_LoadLibrary", shim_SDL_GL_LoadLibrary);
        t.Register(dll, "SDL_GL_GetDrawableSize", shim_SDL_GL_GetDrawableSize);

        t.Register(dll, "SDL_PollEvent", shim_SDL_PollEvent);
        t.Register(dll, "SDL_WaitEvent", shim_SDL_WaitEvent);
        t.Register(dll, "SDL_WaitEventTimeout", shim_SDL_WaitEventTimeout);
        t.Register(dll, "SDL_PushEvent", shim_SDL_PushEvent);
        t.Register(dll, "SDL_GetEventState", shim_SDL_GetEventState);
        t.Register(dll, "SDL_FlushEvents", shim_SDL_FlushEvents);

        t.Register(dll, "SDL_GetTicks", shim_SDL_GetTicks);
        t.Register(dll, "SDL_GetTicks64", shim_SDL_GetTicks64);
        t.Register(dll, "SDL_GetPerformanceCounter", shim_SDL_GetPerformanceCounter);
        t.Register(dll, "SDL_GetPerformanceFrequency", shim_SDL_GetPerformanceFrequency);
        t.Register(dll, "SDL_Delay", shim_SDL_Delay);

        t.Register(dll, "SDL_GetDesktopDisplayMode", shim_SDL_GetDesktopDisplayMode);
        t.Register(dll, "SDL_GetCurrentDisplayMode", shim_SDL_GetCurrentDisplayMode);
        t.Register(dll, "SDL_GetDisplayMode", shim_SDL_GetDisplayMode);
        t.Register(dll, "SDL_GetNumVideoDisplays", shim_SDL_GetNumVideoDisplays);
        t.Register(dll, "SDL_GetNumDisplayModes", shim_SDL_GetNumDisplayModes);
        t.Register(dll, "SDL_GetDisplayBounds", shim_SDL_GetDisplayBounds);
        t.Register(dll, "SDL_GetDisplayDPI", shim_SDL_GetDisplayDPI);

        t.Register(dll, "SDL_OpenAudio", shim_SDL_OpenAudio);
        t.Register(dll, "SDL_OpenAudioDevice", shim_SDL_OpenAudioDevice);
        t.Register(dll, "SDL_CloseAudio", shim_SDL_CloseAudio);
        t.Register(dll, "SDL_CloseAudioDevice", shim_SDL_CloseAudioDevice);
        t.Register(dll, "SDL_PauseAudio", shim_SDL_PauseAudio);
        t.Register(dll, "SDL_PauseAudioDevice", shim_SDL_PauseAudioDevice);
        t.Register(dll, "SDL_QueueAudio", shim_SDL_QueueAudio);
        t.Register(dll, "SDL_GetAudioStatus", shim_SDL_GetAudioStatus);
        t.Register(dll, "SDL_GetAudioDeviceStatus", shim_SDL_GetAudioDeviceStatus);
        t.Register(dll, "SDL_MixAudio", shim_SDL_MixAudio);
        t.Register(dll, "SDL_AudioInit", shim_SDL_AudioInit);
        t.Register(dll, "SDL_AudioQuit", shim_SDL_AudioQuit);
        t.Register(dll, "SDL_GetNumAudioDevices", shim_SDL_GetNumAudioDevices);
        t.Register(dll, "SDL_GetAudioDeviceName", shim_SDL_GetAudioDeviceName);

        t.Register(dll, "SDL_GetKeyboardState", shim_SDL_GetKeyboardState);
        t.Register(dll, "SDL_ShowCursor", shim_SDL_ShowCursor);
        t.Register(dll, "SDL_StartTextInput", shim_SDL_StartTextInput);
        t.Register(dll, "SDL_StopTextInput", shim_SDL_StopTextInput);
        t.Register(dll, "SDL_NumJoysticks", shim_SDL_NumJoysticks);
        t.Register(dll, "SDL_JoystickOpen", shim_SDL_JoystickOpen);
        t.Register(dll, "SDL_GetMouseState", shim_SDL_GetMouseState);
        t.Register(dll, "SDL_GetRelativeMouseState", shim_SDL_GetRelativeMouseState);

        t.Register(dll, "SDL_GetError", shim_SDL_GetError);
        t.Register(dll, "SDL_SetError", shim_SDL_SetError);
        t.Register(dll, "SDL_GetHint", shim_SDL_GetHint);
        t.Register(dll, "SDL_SetHint", shim_SDL_SetHint);
        t.Register(dll, "SDL_GetBasePath", shim_SDL_GetBasePath);
        t.Register(dll, "SDL_GetPrefPath", shim_SDL_GetPrefPath);
        t.Register(dll, "SDL_GetPowerInfo", shim_SDL_GetPowerInfo);
        t.Register(dll, "SDL_GetCPUCount", shim_SDL_GetCPUCount);
        t.Register(dll, "SDL_GetSystemRAM", shim_SDL_GetSystemRAM);
        t.Register(dll, "SDL_GetPlatform", shim_SDL_GetPlatform);
        t.Register(dll, "SDL_GetVersion", shim_SDL_GetVersion);
        t.Register(dll, "SDL_Log", shim_SDL_Log);

        t.Register(dll, "SDL_RWFromFile", shim_SDL_RWFromFile);
        t.Register(dll, "SDL_RWFromMem", shim_SDL_RWFromMem);
        t.Register(dll, "SDL_RWread", shim_SDL_RWread);
        t.Register(dll, "SDL_RWwrite", shim_SDL_RWwrite);
        t.Register(dll, "SDL_RWseek", shim_SDL_RWseek);
        t.Register(dll, "SDL_RWtell", shim_SDL_RWtell);
        t.Register(dll, "SDL_RWsize", shim_SDL_RWsize);
        t.Register(dll, "SDL_RWclose", shim_SDL_RWclose);

        t.Register(dll, "SDL_CreateRGBSurface", shim_SDL_CreateRGBSurface);
        t.Register(dll, "SDL_FreeSurface", shim_SDL_FreeSurface);
        t.Register(dll, "SDL_FillRect", shim_SDL_FillRect);
        t.Register(dll, "SDL_UpperBlit", shim_SDL_UpperBlit);
        t.Register(dll, "SDL_ConvertSurface", shim_SDL_ConvertSurface);
        t.Register(dll, "SDL_LoadBMP_RW", shim_SDL_LoadBMP_RW);
        t.Register(dll, "SDL_LoadWAV_RW", shim_SDL_LoadWAV_RW);
        t.Register(dll, "SDL_FreeWAV", shim_SDL_FreeWAV);
    }
}

}  // namespace hxvm
