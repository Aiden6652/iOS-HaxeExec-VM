// SPDX-License-Identifier: MIT
// 宿主后端接口：垫片层只依赖这套 C 接口，具体由平台侧实现。
//
// iOS 侧：音频走 MobileVLCKit / AVAudioEngine，视频走 MobileVLCKit，
//         渲染走 SDL2-for-iOS（Metal 后端）或 Metal 直出。
// Linux/CLI 侧：全部 no-op，只用来验证链路通不通。
#pragma once

#include <string>

#include "hxvm/basics.hpp"

namespace hxvm {

// 音频 / 视频
struct MediaBackend {
    // 打开文件播放（音乐 / 音效 / 视频）。返回句柄 id（<=0 表示失败）
    i64 (*open)(const std::string &resolved_path, bool loop, bool is_video) = nullptr;
    void (*play)(i64 id) = nullptr;
    void (*pause)(i64 id) = nullptr;
    void (*stop)(i64 id) = nullptr;
    void (*set_volume)(i64 id, float volume) = nullptr;
    void (*set_position)(i64 id, double seconds) = nullptr;
    double (*get_position)(i64 id) = nullptr;
    double (*get_length)(i64 id) = nullptr;
    bool (*is_playing)(i64 id) = nullptr;
    void (*release)(i64 id) = nullptr;
    // 视频专用：把当前帧交给宿主呈现
    void (*set_video_output)(i64 id, void *layer_or_window) = nullptr;
    // 呈现：由渲染后端驱动
};

// 渲染
struct RenderBackend {
    bool (*create_window)(const std::string &title, int w, int h) = nullptr;
    void (*destroy_window)() = nullptr;
    void (*clear)(float r, float g, float b, float a) = nullptr;
    void (*present)() = nullptr;
    void (*resize)(int w, int h) = nullptr;
    // 帧同步钩子：宿主在每一帧刷新前调用，游戏循环靠它对齐 60Hz
    void (*frame_pacing_hook)() = nullptr;
};

void SetMediaBackend(const MediaBackend &b);
const MediaBackend &GetMediaBackend();

void SetRenderBackend(const RenderBackend &b);
const RenderBackend &GetRenderBackend();

// 便利：判断宿主有没有接上真实后端
bool HasMediaBackend();
bool HasRenderBackend();

}  // namespace hxvm
