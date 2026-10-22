// SPDX-License-Identifier: MIT
// 轻量日志：iOS 上走 os_log / NSLog，其他平台走 fprintf(stderr)。
#pragma once

#include <cstdarg>
#include <cstdio>
#include <string_view>

#if defined(__APPLE__)
#include <os/log.h>
#endif

namespace hxvm::log {

enum class Level { Silence = 0, Error, Warn, Info, Trace };

inline Level &g_level() {
    static Level lv = Level::Info;
    return lv;
}

inline void set_level(Level lv) { g_level() = lv; }

inline void emit(Level lv, std::string_view tag, std::string_view msg) {
    if (lv > g_level()) return;
    const char *lvs[] = {"", "E", "W", "I", "T"};
#if defined(__APPLE__)
    os_log_with_type(OS_LOG_DEFAULT, OS_LOG_TYPE_DEFAULT, "%{public}s [%{public}s] %{public}s",
                     lvs[static_cast<int>(lv)], std::string(tag).c_str(), std::string(msg).c_str());
#else
    fprintf(stderr, "%s [%.*s] %.*s\n", lvs[static_cast<int>(lv)],
            static_cast<int>(tag.size()), tag.data(), static_cast<int>(msg.size()), msg.data());
#endif
}

}  // namespace hxvm::log

// 用法：HX_LOGI("loader", "mapped image at %#llx", addr)
#define HX_LOG(level, tag, fmt, ...)                                            \
    do {                                                                        \
        char _hx_buf[1024];                                                     \
        snprintf(_hx_buf, sizeof(_hx_buf), fmt, ##__VA_ARGS__);                 \
        ::hxvm::log::emit(::hxvm::log::Level::level, tag, _hx_buf);             \
    } while (0)

#define HX_LOGE(tag, fmt, ...) HX_LOG(Error, tag, fmt, ##__VA_ARGS__)
#define HX_LOGW(tag, fmt, ...) HX_LOG(Warn, tag, fmt, ##__VA_ARGS__)
#define HX_LOGI(tag, fmt, ...) HX_LOG(Info, tag, fmt, ##__VA_ARGS__)
#define HX_LOGT(tag, fmt, ...) HX_LOG(Trace, tag, fmt, ##__VA_ARGS__)
