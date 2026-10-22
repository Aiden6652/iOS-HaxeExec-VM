#!/usr/bin/env python3
"""
微归因基准：确认 "哪个因素让 x86_64 模拟慢了 100 倍"。

对比同一个 micro() 函数的 9 个变体，各自在 Unicorn 里跑一遍、原生跑一遍，
看 slowdown 随「每次迭代访存次数 / 是否浮点 / 是否写」怎么变化。

用法: python3.12 bench/micro.py
"""
import ctypes
import os
import struct
import subprocess
import time

HERE = os.path.dirname(os.path.abspath(__file__))
from unicorn import Uc, UC_ARCH_X86, UC_MODE_64, UC_PROT_READ, UC_PROT_WRITE, UC_PROT_EXEC
from unicorn.x86_const import (UC_X86_REG_RSP, UC_X86_REG_RDI, UC_X86_REG_RSI,
                               UC_X86_REG_RDX, UC_X86_REG_RAX)

STACK_BASE, STACK_SIZE = 0x7F00_0000_0000, 4 << 20
HEAP_BASE = 0x5000_0000_0000
NodeSize = 8 + 64 + 64 + 8   # next + f[8] + d[8] + visible = 144

VARIANTS = [
    (0, "1× int64 读"),
    (1, "4× int64 读"),
    (2, "8× int64 读"),
    (3, "4× int64 写"),
    (4, "4× 读 + 4× 写 int64"),
    (5, "4× double 读"),
    (6, "4× double 写"),
    (7, "4× 读 + 4× 写 double"),
    (8, "纯寄存器算术（不访存）"),
]


def build():
    src = os.path.join(HERE, "micro.c")
    elf = os.path.join(HERE, "build", "micro.elf")
    so = os.path.join(HERE, "build", "libmicro.so")
    os.makedirs(os.path.join(HERE, "build"), exist_ok=True)
    subprocess.run(["gcc", "-O2", "-nostdlib", "-static", "-no-pie", src, "-o", elf],
                   check=True, capture_output=True)
    subprocess.run(["gcc", "-O2", "-shared", "-fPIC", src, "-o", so],
                   check=True, capture_output=True)
    return elf, so


def elf_loads(path):
    blob = open(path, "rb").read()
    e_phoff, = struct.unpack_from("<Q", blob, 0x20)
    e_phentsize, e_phnum = struct.unpack_from("<HH", blob, 0x36)
    out = []
    for i in range(e_phnum):
        o = e_phoff + i * e_phentsize
        p_type, p_flags, p_offset, p_vaddr, _, p_filesz, p_memsz, _ = \
            struct.unpack_from("<IIQQQQQQ", blob, o)
        if p_type != 1:
            continue
        prot = 0
        prot |= UC_PROT_READ if p_flags & 4 else 0
        prot |= UC_PROT_WRITE if p_flags & 2 else 0
        prot |= UC_PROT_EXEC if p_flags & 1 else 0
        out.append((p_vaddr, p_memsz, blob[p_offset:p_offset + p_filesz], prot))
    return out


def symbols(path):
    out = subprocess.run(["nm", "-S", "--defined-only", path],
                         capture_output=True, text=True, check=True).stdout
    s = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) == 4:
            s[p[3]] = int(p[0], 16)
    return s


def make_nodes(n):
    buf = bytearray()
    for i in range(n):
        nx = (i + 1) * NodeSize if i + 1 < n else 0
        buf += struct.pack("<Q", nx)
        buf += struct.pack("<8q", *([i + 1] * 8))
        buf += struct.pack("<8d", *([1.000001 + i * 1e-9] * 8))
        buf += struct.pack("<q", 1)
    assert len(buf) == n * NodeSize
    return bytes(buf)


def main():
    elf, so = build()
    lib = ctypes.CDLL(so)
    fn = lib.micro
    fn.restype = ctypes.c_uint64
    fn.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_int64]

    uc = Uc(UC_ARCH_X86, UC_MODE_64)
    for vaddr, memsz, data, prot in elf_loads(elf):
        base = vaddr & ~0xFFF
        end = (vaddr + max(memsz, 1) + 0xFFF) & ~0xFFF
        try:
            uc.mem_map(base, end - base, prot)
        except Exception:
            pass
        if data:
            uc.mem_write(vaddr, data)
    uc.mem_map(STACK_BASE, STACK_SIZE, UC_PROT_READ | UC_PROT_WRITE)
    symmicro = symbols(elf)["micro"]

    N = 512
    data = make_nodes(N)
    rsp = STACK_BASE + STACK_SIZE - 0x200
    uc.mem_write(rsp, struct.pack("<Q", 0))

    hbuf = ctypes.create_string_buffer(data, len(data))
    hbase = ctypes.addressof(hbuf)
    for i in range(N):
        off = i * NodeSize
        nx = hbase + (i + 1) * NodeSize if i + 1 < N else 0
        ctypes.cast(hbase + off, ctypes.POINTER(ctypes.c_uint64)).contents.value = nx

    gbuf = HEAP_BASE
    uc.mem_map(gbuf, (len(data) + 0x1000) & ~0xFFF, UC_PROT_READ | UC_PROT_WRITE)
    uc.mem_write(gbuf, data)
    for i in range(N):
        off = gbuf + i * NodeSize
        nx = gbuf + (i + 1) * NodeSize if i + 1 < N else 0
        uc.mem_write(off, struct.pack("<Q", nx))

    print(f"{'变体':<26}{'原生':>10}{'模拟':>12}{'慢多少倍':>10}")
    rows = []
    def reset():
        # 写 workbook 会破坏输入，每次计时前必须复原，否则两侧迭代次数不等价
        ctypes.memmove(hbase, data, len(data))
        uc.mem_write(gbuf, data)
        for i in range(N):
            off = i * NodeSize
            nx_h = hbase + (i + 1) * NodeSize if i + 1 < N else 0
            nx_g = gbuf + (i + 1) * NodeSize if i + 1 < N else 0
            ctypes.cast(hbase + off, ctypes.POINTER(ctypes.c_uint64)).contents.value = nx_h
            uc.mem_write(gbuf + off, struct.pack("<Q", nx_g))

    for vid, desc in VARIANTS:
        iters = 20000
        # native
        best = 1e9
        rn = 0
        for _ in range(3):
            reset()
            t = time.perf_counter()
            rn = fn(vid, ctypes.cast(hbuf, ctypes.c_void_p), iters)
            best = min(best, time.perf_counter() - t)
        # guest
        best_g = 1e9
        rg = 0
        for _ in range(3):
            reset()
            uc.reg_write(UC_X86_REG_RSP, rsp)
            uc.reg_write(UC_X86_REG_RDI, vid)
            uc.reg_write(UC_X86_REG_RSI, gbuf)
            uc.reg_write(UC_X86_REG_RDX, iters)
            t = time.perf_counter()
            try:
                uc.emu_start(symmicro, 0, timeout=0, count=0)
            except Exception:
                pass
            best_g = min(best_g, time.perf_counter() - t)
            rg = uc.reg_read(UC_X86_REG_RAX)
        dt = best_g
        sd = dt / best
        rows.append((desc, best, dt, sd))
        print(f"{desc:<26}{best*1000:>9.2f}ms{dt*1000:>11.2f}ms   x{sd:>6.1f}"
              f"   {'OK' if rg == rn else '结果不一致'}")

    print("\n归因：")
    d = {r[0]: r[3] for r in rows}
    print(f"  1 次读 -> {d['1× int64 读']:.1f}x, 4 次读 -> {d['4× int64 读']:.1f}x, "
          f"8 次读 -> {d['8× int64 读']:.1f}x")
    print(f"  int64 写 -> {d['4× int64 写']:.1f}x, double 写 -> {d['4× double 写']:.1f}x, "
          f"纯寄存器 -> {d['纯寄存器算术（不访存）']:.1f}x")


if __name__ == "__main__":
    main()
