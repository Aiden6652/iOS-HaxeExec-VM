#!/usr/bin/env python3
"""诊断：为什么「写」在 Unicorn 里比「读」慢一个数量级。"""
import ctypes, os, struct, time, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from micro import build, elf_loads, symbols, make_nodes, NodeSize, HEAP_BASE, STACK_BASE, STACK_SIZE
from unicorn import *
from unicorn.x86_const import *

elf, so = build()
lib = ctypes.CDLL(so); fn = lib.micro
fn.restype = ctypes.c_uint64; fn.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_int64]

uc = Uc(UC_ARCH_X86, UC_MODE_64)
for vaddr, memsz, data, prot in elf_loads(elf):
    base = vaddr & ~0xFFF; end = (vaddr + max(memsz, 1) + 0xFFF) & ~0xFFF
    try: uc.mem_map(base, end - base, prot)
    except Exception: pass
    if data: uc.mem_write(vaddr, data)
uc.mem_map(STACK_BASE, STACK_SIZE, UC_PROT_READ | UC_PROT_WRITE)
sym = symbols(elf)["micro"]

N = 8; data = make_nodes(N)
hbuf = ctypes.create_string_buffer(data, len(data)); hb = ctypes.addressof(hbuf)
gbuf = HEAP_BASE
uc.mem_map(gbuf, 0x1000, UC_PROT_READ | UC_PROT_WRITE)
for i in range(N):
    nx_h = hb + (i + 1) * NodeSize if i + 1 < N else 0
    nx_g = gbuf + (i + 1) * NodeSize if i + 1 < N else 0
    ctypes.cast(hb + i * NodeSize, ctypes.POINTER(ctypes.c_uint64)).contents.value = nx_h
    uc.mem_write(gbuf, data)
for i in range(N):
    nx_g = gbuf + (i + 1) * NodeSize if i + 1 < N else 0
    uc.mem_write(gbuf + i * NodeSize, struct.pack("<Q", nx_g))

rsp = STACK_BASE + STACK_SIZE - 0x200
uc.mem_write(rsp, struct.pack("<Q", 0))
cnt = [0]
def hook(uc, addr, size, user): cnt[0] += 1

for vid, iters in ((1, 1000), (3, 1000), (4, 1000), (8, 1000)):
    rn = fn(vid, ctypes.cast(hbuf, ctypes.c_void_p), iters)
    uc.reg_write(UC_X86_REG_RSP, rsp)
    uc.reg_write(UC_X86_REG_RDI, vid)
    uc.reg_write(UC_X86_REG_RSI, gbuf)
    uc.reg_write(UC_X86_REG_RDX, iters)
    cnt[0] = 0
    h = uc.hook_add(UC_HOOK_CODE, hook)
    try: uc.emu_start(sym, 0, timeout=0, count=0)
    except Exception: pass
    uc.hook_del(h)
    rg = uc.reg_read(UC_X86_REG_RAX)
    print(f"variant={vid} iters={iters} host={rn} guest={rg} match={rn == rg} "
          f"guest_instr={cnt[0]}")
