// SPDX-License-Identifier: MIT
// libvlc 垫片。
//
// 目标 EXE 依赖 libvlc.dll 做音频/视频。我们不翻译 VLC 一个字节 ——
// 把它的函数调用全部重定向到宿主的 MobileVLCKit（iOS 侧）。
// 这样解码是原生的，延迟也才是可控的。
#include "hxvm/shim.hpp"

#include <map>
#include <string>

#include "hxvm/log.hpp"
#include "hxvm/process.hpp"
#include "shims/media_backend.hpp"

namespace hxvm {
namespace {

// 模拟的 libvlc 对象。真实工作都转给宿主后端。
struct VlcObject {
    enum Kind { kInstance, kMedia, kPlayer } kind;
    std::string path;      // media 才有
    i64 host_id = 0;       // 宿主媒体句柄
    bool playing = false;
};

std::map<u64, VlcObject> g_objects;
u64 g_next = 1;

u64 NewObject(VlcObject o) {
    const u64 id = g_next++;
    g_objects[id] = o;
    return id;
}

std::string Utf8(const std::u16string &w) {
    std::string s;
    for (char16_t c : w) s.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    return s;
}

// 路径必须过 FileSystem：Windows 路径不区分大小写，iOS 区分
std::string ResolvePath(ShimCall &c, const std::string &raw) {
    std::string r = c.proc->fs().Resolve(raw);
    if (r.empty()) {
        HX_LOGW("vlc", "路径解析失败：%s", raw.c_str());
        r = raw;
    }
    return r;
}

// 所有 libvlc_* 函数的通用骨架
void shim_libvlc_new(ShimCall &c) {
    c.Ret(NewObject({VlcObject::kInstance, "", 0, false}));
}

void shim_libvlc_release(ShimCall &c) {
    g_objects.erase(c.Arg(0));
    c.Ret(0);
}

void shim_libvlc_media_new_path(ShimCall &c) {
    const std::string raw = c.ArgStrA(1);
    const std::string real = ResolvePath(c, raw);
    HX_LOGI("vlc", "加载媒体：%s -> %s", raw.c_str(), real.c_str());
    c.Ret(NewObject({VlcObject::kMedia, real, 0, false}));
}

void shim_libvlc_media_new_location(ShimCall &c) { shim_libvlc_media_new_path(c); }

void shim_libvlc_media_release(ShimCall &c) {
    g_objects.erase(c.Arg(0));
    c.Ret(0);
}

void shim_libvlc_media_player_new(ShimCall &c) {
    c.Ret(NewObject({VlcObject::kPlayer, "", 0, false}));
}

void shim_libvlc_media_player_new_from_media(ShimCall &c) {
    const u64 media = c.Arg(1);
    auto it = g_objects.find(media);
    const std::string path = it != g_objects.end() ? it->second.path : std::string();
    c.Ret(NewObject({VlcObject::kPlayer, path, 0, false}));
}

void shim_libvlc_media_player_release(ShimCall &c) {
    auto it = g_objects.find(c.Arg(0));
    if (it != g_objects.end() && it->second.host_id > 0) {
        const MediaBackend &m = GetMediaBackend();
        if (m.release) m.release(it->second.host_id);
    }
    g_objects.erase(c.Arg(0));
    c.Ret(0);
}

void EnsureHostOpened(ShimCall &c, u64 player) {
    VlcObject &o = g_objects[player];
    if (o.host_id > 0 || o.path.empty()) return;
    const MediaBackend &m = GetMediaBackend();
    if (!m.open) return;
    o.host_id = m.open(o.path, false, false);
    if (o.host_id <= 0) {
        HX_LOGW("vlc", "宿主无法打开媒体：%s", o.path.c_str());
    }
}

void shim_libvlc_media_player_play(ShimCall &c) {
    const u64 p = c.Arg(0);
    EnsureHostOpened(c, p);
    auto it = g_objects.find(p);
    if (it == g_objects.end()) { c.Ret(0); return; }
    const MediaBackend &m = GetMediaBackend();
    if (m.play) m.play(it->second.host_id);
    it->second.playing = true;
    c.Ret(0);
}

void shim_libvlc_media_player_stop(ShimCall &c) {
    auto it = g_objects.find(c.Arg(0));
    if (it != g_objects.end()) {
        const MediaBackend &m = GetMediaBackend();
        if (m.stop) m.stop(it->second.host_id);
        it->second.playing = false;
    }
    c.Ret(0);
}

void shim_libvlc_media_player_pause(ShimCall &c) {
    auto it = g_objects.find(c.Arg(0));
    if (it != g_objects.end()) {
        const MediaBackend &m = GetMediaBackend();
        if (m.pause) m.pause(it->second.host_id);
    }
    c.Ret(0);
}

void shim_libvlc_media_player_set_pause(ShimCall &c) {
    auto it = g_objects.find(c.Arg(0));
    if (it != g_objects.end()) {
        const MediaBackend &m = GetMediaBackend();
        if (c.Arg(1)) {
            if (m.pause) m.pause(it->second.host_id);
        } else {
            if (m.play) m.play(it->second.host_id);
        }
    }
    c.Ret(0);
}

void shim_libvlc_media_player_is_playing(ShimCall &c) {
    auto it = g_objects.find(c.Arg(0));
    if (it == g_objects.end()) { c.Ret(0); return; }
    const MediaBackend &m = GetMediaBackend();
    if (m.is_playing) c.Ret(m.is_playing(it->second.host_id) ? 1 : 0);
    else c.Ret(it->second.playing ? 1 : 0);
}

// 时间：单位是毫秒（libvlc 的时间接口全是 ms）
void shim_libvlc_media_player_get_time(ShimCall &c) {
    auto it = g_objects.find(c.Arg(0));
    if (it == g_objects.end()) { c.Ret(0); return; }
    const MediaBackend &m = GetMediaBackend();
    const double pos = m.get_position ? m.get_position(it->second.host_id) : 0.0;
    c.Ret(static_cast<u64>(pos * 1000.0));
}

void shim_libvlc_media_player_set_time(ShimCall &c) {
    auto it = g_objects.find(c.Arg(0));
    if (it != g_objects.end()) {
        const MediaBackend &m = GetMediaBackend();
        if (m.set_position) m.set_position(it->second.host_id, c.Arg(1) / 1000.0);
    }
    c.Ret(0);
}

void shim_libvlc_media_player_get_length(ShimCall &c) {
    auto it = g_objects.find(c.Arg(0));
    if (it == g_objects.end()) { c.Ret(0); return; }
    const MediaBackend &m = GetMediaBackend();
    const double len = m.get_length ? m.get_length(it->second.host_id) : 0.0;
    c.Ret(static_cast<u64>(len * 1000.0));
}

void shim_libvlc_media_player_get_position(ShimCall &c) {
    auto it = g_objects.find(c.Arg(0));
    if (it == g_objects.end()) { c.Ret(0); return; }
    const MediaBackend &m = GetMediaBackend();
    const double d = m.get_position ? m.get_position(it->second.host_id) : 0.0;
    c.RetDouble(d);
}

// 注意参数位置：libvlc_media_player_set_position(player, double position)
// 在 Windows x64 ABI 下 position 走 XMM1，不是 RDX
void shim_libvlc_media_player_set_position(ShimCall &c) {
    auto it = g_objects.find(c.Arg(0));
    if (it != g_objects.end()) {
        const MediaBackend &m = GetMediaBackend();
        if (m.set_position) m.set_position(it->second.host_id, c.ArgDouble(1));
    }
    c.Ret(0);
}

void shim_libvlc_media_player_get_volume(ShimCall &c) { c.Ret(100); }

void shim_libvlc_media_player_set_volume(ShimCall &c) {
    auto it = g_objects.find(c.Arg(0));
    if (it != g_objects.end()) {
        const MediaBackend &m = GetMediaBackend();
        if (m.set_volume) m.set_volume(it->second.host_id, static_cast<float>(c.Arg(1)) / 100.0f);
    }
    c.Ret(0);
}

void shim_libvlc_audio_set_volume(ShimCall &c) { c.Ret(0); }
void shim_libvlc_video_set_callbacks(ShimCall &c) { c.Ret(0); }
void shim_libvlc_video_set_format(ShimCall &c) { c.Ret(0); }
void shim_libvlc_audio_set_callbacks(ShimCall &c) { c.Ret(0); }
void shim_libvlc_audio_set_format(ShimCall &c) { c.Ret(0); }
void shim_libvlc_media_player_set_hwnd(ShimCall &c) { c.Ret(0); }
void shim_libvlc_media_player_set_nsobject(ShimCall &c) { c.Ret(0); }
void shim_libvlc_media_player_set_xwindow(ShimCall &c) { c.Ret(0); }
void shim_libvlc_media_player_get_state(ShimCall &c) { c.Ret(3); }   // Playing
void shim_libvlc_media_get_duration(ShimCall &c) { c.Ret(0); }
void shim_libvlc_media_player_set_media(ShimCall &c) { c.Ret(0); }
void shim_libvlc_media_player_set_rate(ShimCall &c) { c.Ret(0); }
void shim_libvlc_media_player_get_rate(ShimCall &c) { c.RetDouble(1.0); }
void shim_libvlc_media_add_option(ShimCall &c) { c.Ret(0); }
void shim_libvlc_media_player_set_fullscreen(ShimCall &c) { c.Ret(0); }
void shim_libvlc_errmsg(ShimCall &c) { c.Ret(0); }
void shim_libvlc_clearerr(ShimCall &c) { c.Ret(0); }

}  // namespace

void RegisterVlc(ShimTable &t) {
    const char *k = "libvlc.dll";

    t.Register(k, "libvlc_new", shim_libvlc_new);
    t.Register(k, "libvlc_release", shim_libvlc_release);
    t.Register(k, "libvlc_media_new_path", shim_libvlc_media_new_path);
    t.Register(k, "libvlc_media_new_location", shim_libvlc_media_new_location);
    t.Register(k, "libvlc_media_release", shim_libvlc_media_release);
    t.Register(k, "libvlc_media_add_option", shim_libvlc_media_add_option);
    t.Register(k, "libvlc_media_get_duration", shim_libvlc_media_get_duration);
    t.Register(k, "libvlc_media_player_new", shim_libvlc_media_player_new);
    t.Register(k, "libvlc_media_player_new_from_media", shim_libvlc_media_player_new_from_media);
    t.Register(k, "libvlc_media_player_release", shim_libvlc_media_player_release);
    t.Register(k, "libvlc_media_player_play", shim_libvlc_media_player_play);
    t.Register(k, "libvlc_media_player_stop", shim_libvlc_media_player_stop);
    t.Register(k, "libvlc_media_player_pause", shim_libvlc_media_player_pause);
    t.Register(k, "libvlc_media_player_set_pause", shim_libvlc_media_player_set_pause);
    t.Register(k, "libvlc_media_player_is_playing", shim_libvlc_media_player_is_playing);
    t.Register(k, "libvlc_media_player_get_time", shim_libvlc_media_player_get_time);
    t.Register(k, "libvlc_media_player_set_time", shim_libvlc_media_player_set_time);
    t.Register(k, "libvlc_media_player_get_length", shim_libvlc_media_player_get_length);
    t.Register(k, "libvlc_media_player_get_position", shim_libvlc_media_player_get_position);
    t.Register(k, "libvlc_media_player_set_position", shim_libvlc_media_player_set_position);
    t.Register(k, "libvlc_media_player_get_volume", shim_libvlc_media_player_get_volume);
    t.Register(k, "libvlc_media_player_set_volume", shim_libvlc_media_player_set_volume);
    t.Register(k, "libvlc_media_player_get_state", shim_libvlc_media_player_get_state);
    t.Register(k, "libvlc_media_player_set_media", shim_libvlc_media_player_set_media);
    t.Register(k, "libvlc_media_player_set_rate", shim_libvlc_media_player_set_rate);
    t.Register(k, "libvlc_media_player_get_rate", shim_libvlc_media_player_get_rate);
    t.Register(k, "libvlc_media_player_set_hwnd", shim_libvlc_media_player_set_hwnd);
    t.Register(k, "libvlc_media_player_set_nsobject", shim_libvlc_media_player_set_nsobject);
    t.Register(k, "libvlc_media_player_set_xwindow", shim_libvlc_media_player_set_xwindow);
    t.Register(k, "libvlc_media_player_set_fullscreen", shim_libvlc_media_player_set_fullscreen);
    t.Register(k, "libvlc_audio_set_volume", shim_libvlc_audio_set_volume);
    t.Register(k, "libvlc_audio_set_callbacks", shim_libvlc_audio_set_callbacks);
    t.Register(k, "libvlc_audio_set_format", shim_libvlc_audio_set_format);
    t.Register(k, "libvlc_video_set_callbacks", shim_libvlc_video_set_callbacks);
    t.Register(k, "libvlc_video_set_format", shim_libvlc_video_set_format);
    t.Register(k, "libvlc_errmsg", shim_libvlc_errmsg);
    t.Register(k, "libvlc_clearerr", shim_libvlc_clearerr);
}

}  // namespace hxvm
