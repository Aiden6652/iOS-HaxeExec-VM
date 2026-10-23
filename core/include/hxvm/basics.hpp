// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

namespace hxvm {

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i32 = int32_t;
using i64 = int64_t;
using usize = size_t;
using GuestAddr = u64;

struct HxError : std::runtime_error {
    explicit HxError(const std::string &s) : std::runtime_error(s) {}
};

inline u64 align_up(u64 v, u64 a) { return (v + a - 1) & ~(a - 1); }
inline u64 page_down(u64 v) { return v & ~0xFFFull; }
inline u64 page_up(u64 v) { return align_up(v, 0x1000); }

template <typename T>
T rd(const void *p, size_t off = 0) {
    T v;
    memcpy(&v, static_cast<const u8 *>(p) + off, sizeof(T));
    return v;
}

}  // namespace hxvm
