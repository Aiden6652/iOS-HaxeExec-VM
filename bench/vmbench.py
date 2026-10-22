#!/usr/bin/env python3
"""
iOS-HaxeExec-VM 可行性基准

目的：在写任何量大体之前，先量化 "x86_64 -> Unicorn(TCG) 动态翻译" 的真实代价，
      回答一个工程问题：

      目标游戏（Psych Engine / hxcpp / lime-openfl）在 iOS 上跑 60fps，
      到底有没有可能？

产物：bench/RESULTS.md

用法：  python3.12 bench/vmbench.py            # 跑全部
        python3.12 bench/vmbench.py --quick    # 快速模式
"""

from __future__ import annotations

import argparse
import ctypes
import os
import struct
import subprocess
import sys
import time
from dataclasses import dataclass, field

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

from unicorn import Uc, UC_ARCH_X86, UC_MODE_64, UC_PROT_READ, UC_PROT_WRITE, UC_PROT_EXEC
from unicorn.x86_const import (
    UC_X86_REG_RSP, UC_X86_REG_RDI, UC_X86_REG_RSI, UC_X86_REG_RDX,
    UC_X86_REG_RCX, UC_X86_REG_RAX, UC_X86_REG_RIP, UC_X86_INS_SYSCALL,
)
from unicorn.unicorn_py3 import uc_version
from unicorn.unicorn_const import UC_HOOK_CODE, UC_HOOK_INSN

# ── 来宾进程地址空间布局 ────────────────────────────────────────────────
GUEST_RET_ADDR = 0x0000_1000          # 故意留未映射：ret 之后 fetch 失败 -> 自然停机
STACK_BASE = 0x7F00_0000_0000
STACK_SIZE = 4 * 1024 * 1024
HEAP_BASE = 0x5000_0000_0000

NodeSize = 120        # 见 workloads.c 里的 struct Node

# 走链表、需要在 guest/host 两侧修正 next 指针的一类 workload
OBJ_LIKE = {"bench_objfields", "bench_fields1", "bench_fields8"}

# ─────────────────────────────────────────────────────────────────────────


def build_artifacts() -> tuple[str, str]:
    """编译两份二进制：来宾 ELF（nostdlib，可整体映射）和宿主 .so（原生基线）"""
    src = os.path.join(HERE, "workloads.c")
    elf = os.path.join(HERE, "build", "workloads.elf")
    so = os.path.join(HERE, "build", "libworkloads.so")
    os.makedirs(os.path.join(HERE, "build"), exist_ok=True)

    subprocess.run(
        ["gcc", "-O2", "-nostdlib", "-fno-stack-protector", "-static", "-no-pie",
         src, "-o", elf],
        check=True, capture_output=True,
    )
    subprocess.run(
        ["gcc", "-O2", "-shared", "-fPIC", "-fno-stack-protector", src, "-o", so],
        check=True, capture_output=True,
    )
    return elf, so


def read_elf_loads(path: str):
    """返回 PT_LOAD 列表 [(vaddr, memsz, filesz, data, prot)]"""
    blob = open(path, "rb").read()
    e_phoff = struct.unpack_from("<Q", blob, 0x20)[0]
    e_phentsize, e_phnum = struct.unpack_from("<HH", blob, 0x36)
    loads = []
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type, p_flags, p_offset, p_vaddr, _, p_filesz, p_memsz, _ = struct.unpack_from(
            "<IIQQQQQQ", blob, off)
        if p_type != 1:      # PT_LOAD
            continue
        prot = 0
        prot |= UC_PROT_READ if p_flags & 4 else 0
        prot |= UC_PROT_WRITE if p_flags & 2 else 0
        prot |= UC_PROT_EXEC if p_flags & 1 else 0
        loads.append((p_vaddr, p_memsz, blob[p_offset:p_offset + p_filesz], prot))
    return loads


def read_symbols(path: str):
    out = subprocess.run(["nm", "-S", "--defined-only", path],
                         capture_output=True, text=True, check=True).stdout
    syms = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 4:
            addr, size, typ, name = parts
        elif len(parts) == 3:
            addr, typ, name = parts
            size = "0"
        else:
            continue
        if typ.lower() not in ("t", "d", "b", "r"):
            continue
        syms[name] = int(addr, 16)
    return syms


class GuestVM:
    """一个极简 x86_64 ELF 加载器 + Unicorn 会话（后续 C++ PE loader 的原型）"""

    def __init__(self, elf: str):
        self.uc = Uc(UC_ARCH_X86, UC_MODE_64)
        for vaddr, memsz, data, prot in read_elf_loads(elf):
            base = vaddr & ~0xFFF
            end = (vaddr + max(memsz, 1) + 0xFFF) & ~0xFFF
            try:
                self.uc.mem_map(base, end - base, prot)
            except Exception:
                pass   # 段重叠时忽略
            if data:
                self.uc.mem_write(vaddr, data)
        self.uc.mem_map(STACK_BASE, STACK_SIZE, UC_PROT_READ | UC_PROT_WRITE)
        self.syms = read_symbols(elf)

    def alloc(self, size: int) -> int:
        base = (size + 0xFFF) & ~0xFFF
        p = getattr(self, "_brk", HEAP_BASE)
        self.uc.mem_map(p, max(base, 0x1000), UC_PROT_READ | UC_PROT_WRITE)
        self._brk = p + max(base, 0x1000)
        return p

    def sym(self, name: str) -> int:
        return self.syms[name]

    def call(self, func_addr: int, args, workspace: int = 0, timeout_us: int = 0,
             syscall_hook=None) -> tuple[int, float]:
        """调用来宾函数，返回 (rax, 耗时秒)"""
        uc = self.uc
        rsp = STACK_BASE + STACK_SIZE - 0x200
        uc.reg_write(UC_X86_REG_RSP, rsp)
        uc.mem_write(rsp, struct.pack("<Q", GUEST_RET_ADDR))
        regs = [UC_X86_REG_RDI, UC_X86_REG_RSI, UC_X86_REG_RDX, UC_X86_REG_RCX]
        for r, v in zip(regs, args):
            uc.reg_write(r, v)
        hook = None
        if syscall_hook:
            hook = uc.hook_add(UC_HOOK_INSN, syscall_hook, None, 1, 0, UC_X86_INS_SYSCALL)

        t0 = time.perf_counter()
        try:
            uc.emu_start(func_addr, 0, timeout=timeout_us, count=0)
        except Exception:
            pass   # 撞到未映射的返回地址 => 正常返回路径
        dt = time.perf_counter() - t0
        if hook is not None:
            uc.hook_del(hook)
        return uc.reg_read(UC_X86_REG_RAX), dt


# ── 构造共享测试用例 ────────────────────────────────────────────────────

def make_node_list(n: int) -> bytes:
    buf = bytearray()
    for i in range(n):
        nx = (i + 1) * NodeSize if i + 1 < n else 0
        buf += struct.pack("<qqqq", nx, i, 2 * i, 3 * i)      # next,x,y,scaleMode
        buf += struct.pack("<dddd", 0.5, 1.000001, 1.000002, 0.0)  # a,sx,sy,rot
        buf += struct.pack("<qq", 1, 0)                        # visible,pad0
        buf += b"\x00" * 40
    assert len(buf) == n * NodeSize, (len(buf), n * NodeSize)
    return bytes(buf)


def link_nodes_host(base: int, n: int) -> None:
    """把链表里的相对 next 改成宿主绝对地址（Guest 侧同样要改）"""
    for i in range(n):
        off = i * NodeSize
        nx = base + (i + 1) * NodeSize if i + 1 < n else 0
        ptr = ctypes.cast(base + off, ctypes.POINTER(ctypes.c_uint64))
        ptr.contents.value = nx


def make_u64_arena(words: int, span: int) -> bytes:
    """伪随机指针追赶数组：值域限制在 [0, span)"""
    buf = bytearray(words * 8)
    v = 123456789
    import numpy as np
    arr = np.empty(words, dtype=np.uint64)
    for i in range(words):
        v = (v * 6364136223846793005 + 1442695040888963407) & 0xFFFFFFFFFFFFFFFF
        arr[i] = v % span
    return arr.tobytes()


def make_matrix_pool(count: int) -> bytes:
    buf = bytearray()
    import numpy as np
    for i in range(count):
        m = (np.arange(16, dtype=np.float64).reshape(4, 4) + i) * 0.01
        buf += m.tobytes()
    return bytes(buf)


def make_pcm(count_elements: int) -> bytes:
    import numpy as np
    a = (np.arange(count_elements, dtype=np.int32) % 20000 - 10000).astype(np.int16)
    return a.tobytes()


# ── workload 定义 ───────────────────────────────────────────────────────

@dataclass
class Workload:
    name: str
    desc: str
    knobs: dict = field(default_factory=dict)
    result: dict = field(default_factory=dict)


WORKLOADS = [
    Workload("bench_ptrchase", "指针追逐 / L1 内（计算受限上界）",
             {"words": 1 << 16, "span": 1 << 16}),
    Workload("bench_ptrchase", "指针追逐 / 8MB（缺失主导，乐观下界）",
             {"words": 1 << 20, "span": 1 << 20}),
    Workload("bench_fields1", "对照：每节点只访问 1 个字段",
             {"nodes": 4096, "iters": 2000}),
    Workload("bench_objfields", "DisplayObject 链表逐帧 update（6 字段）",
             {"nodes": 4096, "iters": 2000}),
    Workload("bench_fields8", "对照：每节点访问 8 个字段",
             {"nodes": 4096, "iters": 2000}),
    Workload("bench_strings", "UTF-16 资源 key hash（FNV-1a）",
             {"words": 512, "reps": 200000}),
    Workload("bench_matrix", "精灵 4x4 transform 合成（double）",
             {"count": 1024, "reps": 512}),
    Workload("bench_pcmix", "32 路 PCM 混音（SDL audio callback）",
             {"frames": 1 << 17, "nchans": 32}),
]

TARGET_NATIVE_SEC = 0.12


def native_fn(so, name):
    lib = ctypes.CDLL(so)
    fn = getattr(lib, name)
    fn.restype = ctypes.c_uint64
    fn.argtypes = [ctypes.c_void_p, ctypes.c_int64, ctypes.c_int64]
    return fn


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--target", type=float, default=TARGET_NATIVE_SEC)
    args = ap.parse_args()
    if args.quick:
        args.target = 0.03

    print("== iOS-HaxeExec-VM 可行性基准 ==")
    print(f"Unicorn {uc_version()}, host {os.uname().machine}, {os.cpu_count()} cores")
    elf, so = build_artifacts()
    vm = GuestVM(elf)

    rows = []

    for w in WORKLOADS:
        fn = native_fn(so, w.name)

        # ── 构造输入 ──
        if w.name == "bench_ptrchase":
            words = w.knobs["words"]
            data = make_u64_arena(words, w.knobs["span"])
            nBase = 1 << 20
        elif w.name in OBJ_LIKE:
            data = make_node_list(w.knobs["nodes"])
            iters = w.knobs["iters"]
            # 修 Guest 里的 next 指针（相对地址 -> 绝对地址）
        elif w.name == "bench_strings":
            data = make_u64_arena(w.knobs["words"], w.knobs["words"] // 2)
        elif w.name == "bench_matrix":
            data = make_matrix_pool(w.knobs["count"])
        else:
            frames = w.knobs["frames"]
            data = make_pcm(frames * w.knobs["nchans"])

        # ── 原生基准 + 自动标定规模 ──
        nbuf_h = ctypes.create_string_buffer(data, len(data))
        host_base = ctypes.addressof(nbuf_h)
        if w.name in OBJ_LIKE:
            link_nodes_host(host_base, w.knobs["nodes"])

        def run_native(scale):
            if w.name == "bench_ptrchase":
                return fn(ctypes.cast(nbuf_h, ctypes.c_void_p), scale, 0)
            if w.name in OBJ_LIKE:
                return fn(ctypes.cast(nbuf_h, ctypes.c_void_p), scale, 0)
            if w.name == "bench_strings":
                return fn(ctypes.cast(nbuf_h, ctypes.c_void_p),
                          w.knobs["words"], scale)
            if w.name == "bench_matrix":
                return fn(ctypes.cast(nbuf_h, ctypes.c_void_p),
                          w.knobs["count"], scale)
            return fn(ctypes.cast(nbuf_h, ctypes.c_void_p),
                      w.knobs["frames"], w.knobs["nchans"])

        base_scale = {"bench_ptrchase": 1 << 18, "bench_fields1": 512,
                      "bench_objfields": 32, "bench_fields8": 32,
                      "bench_strings": 20000, "bench_matrix": 64,
                      "bench_pcmix": 1}[w.name]
        t0 = time.perf_counter()
        r0 = run_native(base_scale)
        dt0 = time.perf_counter() - t0
        if w.name == "bench_pcmix":
            scale = base_scale
        else:
            scale = max(1, int(base_scale * args.target / max(dt0, 1e-6)))

        def reset_host():
            ctypes.memmove(host_base, data, len(data))
            if w.name in OBJ_LIKE:
                link_nodes_host(host_base, w.knobs["nodes"])

        best = 1e9
        r_native = 0
        for _ in range(3):
            reset_host()
            t0 = time.perf_counter()
            r_native = run_native(scale)
            best = min(best, time.perf_counter() - t0)
        native_sec = best

        # ── 模拟执行 ──
        gbuf = vm.alloc(len(data) + 0x10000)
        vm.uc.mem_write(gbuf, data)
        if w.name in OBJ_LIKE:
            # 修正链表里的 next 指针为来宾绝对地址
            n = w.knobs["nodes"]
            for i in range(n):
                off = gbuf + i * NodeSize
                nx = gbuf + (i + 1) * NodeSize if i + 1 < n else 0
                vm.uc.mem_write(off, struct.pack("<Q", nx))
            iters = scale

        if w.name == "bench_ptrchase":
            gargs = (gbuf, scale, 0)
        elif w.name in OBJ_LIKE:
            gargs = (gbuf, iters, 0)
        elif w.name == "bench_strings":
            gargs = (gbuf, w.knobs["words"], scale)
        elif w.name == "bench_matrix":
            gargs = (gbuf, w.knobs["count"], scale)
        else:
            gargs = (gbuf, w.knobs["frames"], w.knobs["nchans"])

        best_g = 1e9
        r_guest = 0
        for _ in range(3):
            vm.uc.mem_write(gbuf, data)
            if w.name in OBJ_LIKE:
                n = w.knobs["nodes"]
                for i in range(n):
                    off = gbuf + i * NodeSize
                    nx = gbuf + (i + 1) * NodeSize if i + 1 < n else 0
                    vm.uc.mem_write(off, struct.pack("<Q", nx))
            rg, dtg = vm.call(vm.sym(w.name), gargs)
            best_g = min(best_g, dtg)
            r_guest = rg
        emu_sec = best_g
        ok = (r_guest == r_native)
        slowdown = emu_sec / max(native_sec, 1e-9)
        rows.append((w.desc, w.name, scale, native_sec, emu_sec, slowdown, ok))
        print(f"{w.desc:<38} native={native_sec*1000:8.2f}ms "
              f"emu={emu_sec*1000:9.2f}ms  x{slowdown:6.1f}  match={'OK' if ok else 'X'}")

    # ── 7) VM 退出代价 ──
    exits = 20000
    n_sys = [0]

    def on_syscall(uc, user):
        n_sys[0] += 1
        uc.reg_write(UC_X86_REG_RAX, 1)
        uc.reg_write(UC_X86_REG_RIP, uc.reg_read(UC_X86_REG_RIP) + 2)

    _, emu_exit = vm.call(vm.sym("bench_exitloop"), (exits, 0, 0),
                          syscall_hook=on_syscall)
    per_exit_emu = emu_exit / max(n_sys[0], 1) * 1e6
    fn = native_fn(so, "bench_exitloop")
    best = 1e9
    for _ in range(3):
        t0 = time.perf_counter()
        fn(None, exits, 0)
        best = min(best, time.perf_counter() - t0)
    per_exit_native = best / exits * 1e6
    print(f"{'VM 退出（每次导入函数调用）':<38} native={per_exit_native:8.3f}us "
          f"emu={per_exit_emu:9.3f}us")

    # ── 输出报告 ──
    write_report(rows, per_exit_native, per_exit_emu, args.quick)
    print("\n已写出 bench/RESULTS.md")


def write_report(rows, native_exit, emu_exit, quick):
    p = os.path.join(HERE, "RESULTS.md")
    with open(p, "w", encoding="utf-8") as f:
        f.write("# Unicorn x86_64 模拟性能实测\n\n")
        f.write(f"- 宿主：`{os.uname().machine}` Linux，Unicorn {uc_version()}\n")
        f.write("- 方法：同一份 `bench/workloads.c`，一次编译为 x86_64 ELF 在 Unicorn 里执行，"
                "一次编译为宿主原生 `.so` 直接调用，取相同参数的墙钟时间比。\n")
        f.write("- 结论口径：**slowdown = 模拟耗时 / 原生耗时**，"
                "直接等于「每秒能跑多少帧」的折损倍数。\n\n")

        f.write("| workload | 原生 | 模拟 | 慢多少倍 | 结果一致 |\n")
        f.write("|---|---:|---:|---:|---|\n")
        for desc, name, scale, ns, es, sd, ok in rows:
            f.write(f"| {desc} | {ns*1000:.2f} ms | {es*1000:.2f} ms | **x{sd:.1f}** | "
                    f"{'✅' if ok else '❌'} |\n")
        f.write(f"\nVM 退出代价（一次外部函数调用 = Windows 导入函数调用一次）："
                f"原生 {native_exit:.3f} µs / 模拟后 {emu_exit:.3f} µs\n\n")

        worst = max(r[5] for r in rows)
        best = min(r[5] for r in rows)
        f.write("## 这意味着什么\n\n")
        f.write(f"纯 x86_64 用户态代码走 TCG 动态重编译，本机的实测折损是 "
                f"**x{best:.1f} ~ x{worst:.1f}**。\n\n")
        f.write("把它换算成帧预算（16.67 ms / 帧）：\n\n")
        f.write("| 假设原生帧耗时 | x5 | x10 | x20 | x40 |\n")
        f.write("|---|---:|---:|---:|---:|\n")
        f.write("| 4 ms (250fps 原生) | 20 ms | 40 ms | 80 ms | 160 ms |\n")
        f.write("| 8 ms (125fps 原生) | 40 ms | 80 ms | 160 ms | 320 ms |\n")
        f.write("\n> 只有原生帧耗时 < 3 ms 的类型才有机会在 x5 下摸到 60fps，"
                "而 Psych Engine 的 update+render 远不止这个量级。\n")
    print(open(p).read()[:200])


if __name__ == "__main__":
    main()
