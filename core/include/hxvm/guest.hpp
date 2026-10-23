// SPDX-License-Identifier: MIT
// 来宾地址空间。所有跨模块读写都必须经过这里 —— 这样后面把内存模型换成
// 「直接映射」时，只需要换实现，调用方一行都不用改。
#pragma once

#include <string>
#include <vector>

#include "hxvm/basics.hpp"

namespace hxvm {

enum ProtBits { kProtNone = 0, kProtRead = 1, kProtWrite = 2, kProtExec = 4 };

class GuestMemory {
public:
    virtual ~GuestMemory() = default;

    virtual bool Map(u64 addr, usize size, u32 prot) = 0;
    virtual bool Protect(u64 addr, usize size, u32 prot) = 0;
    virtual bool IsMapped(u64 addr) = 0;
    virtual bool Write(u64 addr, const void *data, usize n) = 0;
    virtual bool Read(u64 addr, void *data, usize n) const = 0;
    virtual bool Fill(u64 addr, u8 value, usize n) = 0;

    // ── 标量 ────────────────────────────────────────────────────────
    u8 Read8(u64 a) const { u8 v = 0; Read(a, &v, 1); return v; }
    u16 Read16(u64 a) const { u16 v = 0; Read(a, &v, 2); return v; }
    u32 Read32(u64 a) const { u32 v = 0; Read(a, &v, 4); return v; }
    u64 Read64(u64 a) const { u64 v = 0; Read(a, &v, 8); return v; }

    void Write8(u64 a, u8 v) { Write(a, &v, 1); }
    void Write16(u64 a, u16 v) { Write(a, &v, 2); }
    void Write32(u64 a, u32 v) { Write(a, &v, 4); }
    void Write64(u64 a, u64 v) { Write(a, &v, 8); }

    // ── 字符串：Windows 侧 A 版 = 多字节，W 版 = UTF-16LE ────────────
    std::string ReadCString(u64 a, usize max = 4096) const;
    std::u16string ReadWString(u64 a, usize max = 4096) const;
    void WriteCString(u64 a, const std::string &s);
    void WriteWString(u64 a, const std::u16string &s);

    u64 ReadPtr(u64 a) const { return Read64(a); }
    void WritePtr(u64 a, u64 v) { Write64(a, v); }
};

// 来宾堆：块必须落在来宾地址空间内 —— 来宾代码会直接往里写，
// 不能像常见模拟器那样只在宿主侧 malloc 了事。
class GuestHeap {
public:
    void Init(GuestMemory &mem, u64 base, usize size);
    u64 Alloc(usize size, bool zero = false);
    bool Free(u64 addr);
    usize SizeOf(u64 addr) const;

private:
    struct Block { u64 addr; usize size; bool used; };
    GuestMemory *mem_ = nullptr;
    u64 base_ = 0;
    usize size_ = 0;
    u64 brk_ = 0;
    std::vector<Block> blocks_;
};

}  // namespace hxvm
