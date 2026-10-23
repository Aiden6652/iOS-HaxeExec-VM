// SPDX-License-Identifier: MIT
#include "hxvm/guest.hpp"

#include <algorithm>
#include <cstring>

namespace hxvm {

std::string GuestMemory::ReadCString(u64 a, usize max) const {
    std::string out;
    u8 buf[256];
    usize pos = 0;
    while (pos < max) {
        usize chunk = std::min<usize>(sizeof(buf), max - pos);
        if (!Read(a + pos, buf, chunk)) break;
        bool hit_nul = false;
        for (usize i = 0; i < chunk; i++) {
            if (buf[i] == 0) { hit_nul = true; break; }
            out.push_back(static_cast<char>(buf[i]));
        }
        if (hit_nul) break;
        pos += chunk;
    }
    return out;
}

std::u16string GuestMemory::ReadWString(u64 a, usize max) const {
    std::u16string out;
    u16 buf[128];
    usize pos = 0;
    while (pos < max) {
        usize chunk = std::min<usize>(sizeof(buf) / 2, max - pos);
        if (!Read(a + pos, buf, chunk * 2)) break;
        bool hit_nul = false;
        for (usize i = 0; i < chunk; i++) {
            if (buf[i] == 0) { hit_nul = true; break; }
            out.push_back(static_cast<char16_t>(buf[i]));
        }
        if (hit_nul) break;
        pos += chunk;
    }
    return out;
}

void GuestMemory::WriteCString(u64 a, const std::string &s) {
    if (!s.empty()) Write(a, s.data(), s.size());
    Write8(a + s.size(), 0);
}

void GuestMemory::WriteWString(u64 a, const std::u16string &s) {
    for (usize i = 0; i < s.size(); i++) Write16(a + i * 2, static_cast<u16>(s[i]));
    Write16(a + s.size() * 2, 0);
}

// ── 来宾堆 ────────────────────────────────────────────────────────────

void GuestHeap::Init(GuestMemory &mem, u64 base, usize size) {
    mem_ = &mem;
    base_ = base;
    size_ = size;
    brk_ = base;
    blocks_.clear();
}

u64 GuestHeap::Alloc(usize want, bool zero) {
    if (!mem_) return 0;
    usize size = align_up(want + 16, 16);

    // 先找空闲块复用
    for (auto &b : blocks_) {
        if (!b.used && b.size >= size) {
            b.used = true;
            if (zero) mem_->Fill(b.addr, 0, size);
            return b.addr;
        }
    }
    // 不够就往上推 brk
    if (brk_ + size > base_ + size_) return 0;
    u64 addr = brk_;
    brk_ += size;
    blocks_.push_back({addr, size, true});
    if (zero) mem_->Fill(addr, 0, size);
    return addr;
}

bool GuestHeap::Free(u64 addr) {
    for (auto &b : blocks_) {
        if (b.used && b.addr == addr) {
            b.used = false;
            return true;
        }
    }
    return false;
}

usize GuestHeap::SizeOf(u64 addr) const {
    for (const auto &b : blocks_) {
        if (b.used && b.addr == addr) return b.size;
    }
    return 0;
}

}  // namespace hxvm
