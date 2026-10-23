# iOS-HaxeExec-VM

在 **非越狱 iOS（ARM64）** 上原生运行一个 **闭源、无源码的 x86_64 Windows EXE**
（hxcpp 编译的 Psych Engine 深度魔改版）。做法是 **JIT 权限 + 用户态指令翻译 + Win32 垫片层**，
不跑整个 Windows，也不做系统级虚拟化。

> 仓库里最贵的东西不是代码，是**两组实测数字**：一组决定了这个项目的架构必须怎么改，
> 一组决定了它的上限。先读 [bench/RESULTS.md](bench/RESULTS.md) 和下面的「性能判决」。

---

## 现状：能跑

`hxvm-cli` 已经能在开发机上把真正的 PE32+ x86-64 控制台程序跑完（退出码、堆、文件、
注册表、目录枚举全部按 Windows 语义工作）。CI 里每次 push 都会自动复验：

```
=== 来宾程序启动 ===
  [OK] 打开 Icon.PNG，大小 24，读到 24 字节：PNGDATA-HELLO-FROM-GUEST
  [OK] 全大写路径 ASSETS/songs/week1/ICON.png
  [OK] 缺失文件按预期失败
  [OK] HeapAlloc -> 非 0
  [OK] QPC=47231 频率=10000000
  [OK] 注册表 ProductName=Windows 10 Pro
  [OK] FindNextFile 又枚举了 2 项
=== 来宾程序结束，返回 42 ===
```

来宾程序故意用 `assets\Songs\WEEK1\Icon.PNG` 去请求磁盘上的 `assets/songs/week1/icon.png`
—— 大小写和分隔符全都对不上，靠垫片层兜住了。

**还没做**：绘制命令转发（画面是空的）、音频/视频的宿主实现、触控注入。

---

## 性能判决（先说结论，别急着堆功能）

| 操作形态 | 折损 |
|---|---:|
| 纯寄存器算术（不访存） | **x1.1** |
| int64 读 | x3.4 |
| double 读 | x21.8 |
| int64 **写** | **x42.2** |
| double **写** | **x76.5** |
| DisplayObject 链表逐帧 update | **x100+** |
| 精灵 transform 合成 | **x123** |

**翻译器本身几乎不慢（1.1x），代价全在访存。** Unicorn 走 soft-MMU，
每条来宾访存指令都要进 helper + TLB 查表，而**写比读贵一个数量级**。
hxcpp 生成的代码形态就是密集的堆对象字段读改写，正好踩在最贵的那条路上。

本机和 GitHub CI（不同 CPU、不同核数）跑出来的倍数几乎一样 —— 说明瓶颈不在算力，
在内存模型。这条结论是目前全部架构决策的依据。

> **按当前 Unicorn 内核，成品是 2~6 fps。60fps 不可达，差距是数量级。**

**要提速，换的不是引擎品牌，是内存模型**：

| | Unicorn（当前） | qemu-user / Box64 式 dynarec |
|---|---|---|
| 来宾访存 | soft-MMU，每次过 helper | 来宾内存**直接映射**进宿主地址空间 |
| load/store | x3 ~ x80 | ≈ 原生单条指令 |
| 预期折损 | x40 ~ x130 | x3 ~ x8 |

所以 CPU 后端被抽象成了 `ICpuBackend`（`core/include/hxvm/cpu.hpp`），
Unicorn 只是当前实现。换成直接映射 dynarec 时，**垫片层一行都不用改** ——
这是本仓库里最关键的结构性决定。

---

## 结构

```
core/          可换后端的执行核心
  include/hxvm/cpu.hpp         ICpuBackend 抽象（换内核就换实现）
  include/hxvm/guest.hpp       来宾地址空间 + 来宾堆
  include/hxvm/loader.hpp      PE32+ 加载器（含导入表打桩）
  include/hxvm/shim.hpp        Win32 垫片表 + thunk 发射
  include/hxvm/filesystem.hpp  大小写不敏感路径解析
  include/hxvm/registry.hpp    内存假注册表
  src/cpu_unicorn.cpp          Unicorn 后端
shims/         kernel32 / user32 / advapi32 / msvcrt(CRT) / libvlc / SDL2
host/cli.cpp   开发机命令行宿主
platform/ios/  iOS 宿主（CADisplayLink + CAMetalLayer + entitlements）
tests/         mingw 来宾测试程序 + 测试资源
bench/         性能基准（归因 + 端到端）
```

### 导入调用是怎么被拦下来的

IAT 的每个槽位都被改写成一个 16 字节 thunk：

```
48 B8 <slot|magic>   movabs rax, slot
0F 05                syscall      ← 后端在这里截断并分发
C3                   ret          ← 正常返回来宾调用者
90 90 90             nop（对齐）
```

用 `syscall` 而不是软中断，是因为 Unicorn 对它有专门的 `UC_HOOK_INSN` 支持，
代价是已知量，且不依赖特权指令。

---

## 踩过的坑（都是真金白银换来的）

这几个坑的共同点是：**现象离根因很远**，值得记下来。

1. **Unicorn 的 `UC_HOOK_INSN` 回调不传 `user_data`**
   实测拿到的是栈上的垃圾。改为自建 `uc_engine* → backend` 注册表，回调里按 `uc` 查回对象。

2. **不要在 syscall hook 里手动 `RIP += 2`**
   Unicorn 回调返回后会自己把 RIP 推进过 syscall（2 字节），正好落在 thunk 的 `ret` 上。
   再手动 +2 就跳过了 `ret`，执行流顺着 nop 滑进下一个 thunk，栈从此失衡。
   现象：程序不崩，但跑几百条指令后跳到莫名其妙的地址。

3. **PE 导入表里不全是函数**
   msvcrt 会把 `_fmode` / `__initenv` 这类**全局变量**也导出为导入项。
   当成函数打桩后，CRT 会读到 thunk 的机器码，然后必崩。
   修法是在来宾内存里给它们开真正的变量槽，IAT 槽位写「槽的地址」。
   反向的坑也一样疼：`_errno` 在 mingw 里是**函数** `int* _errno(void)`，
   把它当变量处理，CRT 就会跳到数据区上去执行。

4. **`HKEY_LOCAL_MACHINE` 实际是 `0xFFFFFFFF80000002`**
   SDK 里这么写的：`((HKEY)(ULONG_PTR)((LONG)0x80000002))` —— 先当 32 位有符号数，
   再做符号扩展。只比较低 32 位才能稳定匹配，否则全部掉进 default 分支。

5. **`HeapAlloc(heap, flags, bytes)` 是 3 个参数**
   按 4 个读会读到栈上的垃圾，现象是「堆分配永远返回 NULL」。

6. **失败时别返回 0**
   Win32 的 `0 == ERROR_SUCCESS`。`RegOpenKeyExA` 打开失败也返回 0 的话，
   调用方会以为成功了，然后拿一个野句柄去查值 —— 现象是「注册表静悄悄不工作」。

---

## 渲染边界（重要）

原方案里写的是「拦截 FrameBuffer 写入再贴到 CAMetalLayer」。**这个方向不对**：
OpenFL/lime 在 Windows 上走 SDL2 + OpenGL（经 ANGLE 转 D3D），
目标 EXE 几乎一定会 import `SDL2.dll`。

正确做法是在 **SDL2 边界** 整个换掉 —— iOS 官方 SDL2 有 Metal 后端，
能直接拿硬件加速。抓屏只作为所有这些都接不上时的最后兜底。

帧同步也因此可以不靠猜：来宾调用 `SDL_RenderPresent` / `SDL_GL_SwapWindow`
就等于这一帧结束了（`Process::RequestFrameDone()`），
宿主立刻收工去上屏，天然对齐刷新率。

---

## 用法

```bash
# 基准（架构决策的依据）
pip install unicorn capstone numpy
python bench/micro.py      # 归因：读 / 写 / 纯寄存器
python bench/vmbench.py    # 端到端 workload

# 在开发机上跑一个 Windows EXE
cmake -G Ninja -B build -S . -DCMAKE_BUILD_TYPE=Release \
      -DUNICORN_ROOT="$(python -c 'import unicorn,os;print(os.path.dirname(unicorn.__file__))')"
ninja -C build
./build/hxvm-cli game.exe ./assets
```

## CI

| workflow | 作用 |
|---|---|
| `bench.yml` | 每次 push 重跑性能基准，并校验结论仍然成立 |
| `e2e.yml` | 用 mingw 现编一个真 Windows EXE，放进 VM 跑，逐项断言 |
| `ipa.yml` | macOS runner：先交叉编译 iOS arm64 的 Unicorn，再打包 **未签名 IPA** |

产出的 IPA 是未签名的 —— CI 上没有 Apple 账号，
装到设备上之前需要自己重签（SideStore / AltStore / 开发者证书）。

## 下一步

1. **换内核**（决定上限的那一步）：把 `ICpuBackend` 换成直接映射 dynarec，
   验证「写」能否从 42x 压到个位数。在此之前，所有性能优化都是空转。
2. **渲染转发**：在 SDL2 垫片里把绘制命令转给 iOS SDL2 的 Metal 后端。
3. **音频**：`libvlc` 垫片已经把调用转到 `MediaBackend` 接口，
   接上 MobileVLCKit / AVAudioEngine 即可。
4. **触控注入**（用户明确说先跳过）：坐标直接写 guest 内存里的按键状态位，不挂起线程。
