# iOS-HaxeExec-VM

把一个 **闭源、无源码的 x86_64 Windows 可执行文件**（hxcpp 编译的 Psych Engine 深度魔改版）
搬到非越狱 iOS（ARM64）上原生跑 —— 靠的是 **JIT 权限 + 用户态指令翻译 + Win32 API 垫片层**。

> **这个仓库目前最有价值的产出不是代码，是一组实测数字。**
> 它们决定了「能不能跑到 60fps」这个问题只有一个答案，也决定了整个架构该怎么改。
> 先读 [bench/RESULTS.md](bench/RESULTS.md)。

---

## 一句话结论

**原方案（Unicorn + 完整 Win32 垫片）做不到 60fps，差距是数量级不是百分比。**

原因不在「垫片层工程量不够」，也不在渲染方案，而在 **内存模型**：
Unicorn 走 soft-MMU，每条来宾访存指令都要进 helper + TLB 查表，而 **写操作比读贵一个数量级**。
hxcpp 生成的代码形态恰恰是「密集的堆对象字段读改写」，正好踩在最贵的那条路上。

### 实测数据

归因基准（`bench/micro.py`，Unicorn 2.1.4 / Linux x86_64）：

| 操作形态 | 原生 | 模拟 | 折损 |
|---|---:|---:|---:|
| 纯寄存器算术（不访存） | 0.27 ms | 0.29 ms | **x1.1** |
| 4× int64 读 | 23.4 ms | 80.2 ms | x3.4 |
| 4× double 读 | 18.0 ms | 391.2 ms | x21.8 |
| 4× int64 **写** | 28.3 ms | 1196.3 ms | **x42.2** |
| 4× double **写** | 20.2 ms | 1546.7 ms | **x76.5** |

真实 workload（`bench/vmbench.py`，结果全部校验一致）：

| workload | 折损 |
|---|---:|
| DisplayObject 链表逐帧 update | **x100.6** |
| 精灵 4x4 transform 合成 | **x123.2** |
| 32 路 PCM 混音 | x26.3 |
| UTF-16 资源 key hash | x2.6 |

**翻译器本身几乎不慢（1.1x）。代价全在访存。**

一帧原生 4 ms → 模拟后 160~520 ms → **2~6 fps**。这还没算 iOS 单核性能、x86_64→ARM64 更大的指令展开比、
以及渲染 API 转发的 VM 退出成本（Python 绑定下实测 111 µs/次，C++ 下会低两个数量级但仍是主成本之一）。

---

## 据此修正后的架构

要追「流畅」，必须换掉的是 **内存模型**，而不是引擎的品牌：

| | Unicorn（原来） | qemu-user / Box64 式 dynarec（该走的） |
|---|---|---|
| 来宾访存 | soft-MMU，每次都过 helper | 来宾内存 **直接映射**进宿主地址空间 |
| load/store 成本 | x3 ~ x80 | ≈ 原生单条指令 |
| 预期整体折损 | x40 ~ x130 | **x3 ~ x8** |

换成直接映射式 dynarec 之后：是 10~20fps 而不是 2fps。**依然到不了 60 —— 这是留给这个项目唯一的、
也是最硬的约束，任何绕开它的努力都是空转。**

其余部分是常规工程量，按优先级排：

1. **核心**：CPU 后端做成可替换抽象（`core/include/hxvm/cpu.hpp`），Unicorn 只是当前实现，
   便于后续整体换成直接映射 dynarec 而 **不动垫片层**。
2. **PE 加载器**：PE32+ 解析、节映射、导入表重写（IAT 打桩成 `movabs rax, slot; syscall; ret` 的 thunk）。
3. **Win32 垫片**：
   - `kernel32`：路径大小写不敏感 + `\`→`/` 重定向（一次性建全盘索引）、`HeapAlloc`→来宾堆、
     `QueryPerformanceCounter`→`mach_absolute_time`
   - `user32`：`MessageBoxA`→no-op / `UIAlertController`
   - `advapi32`：内存里 `unordered_map` 伪造注册表
   - `libvlc`：重定向到 MobileVLCKit
4. **渲染**：石灰/openfl 在 Windows 上经由 ANGLE 上 D3D，**原.exe 一定会 import `SDL2.dll` /
   `libEGL` / `libGLESv2`**。所以优先 shim 在 **SDL2 边界**（iOS 官方 SDL2 有 Metal 后端），
   拿硬件加速；把「FrameBuffer 抓屏」当最后的兜底，不是首选。
5. **帧同步**：`CADisplayLink` 驱动，虚拟时钟与刷新率对齐。
6. **输入**：触摸坐标直写 Guest 内存里的按键状态位，不挂起线程。

---

## 仓库里现在有什么

```
bench/
  workloads.c       用 -nostdlib 编成 ELF 在 Unicorn 里跑的 workload
  micro.c           访存归因对照实验（读/写/纯寄存器）
  vmbench.py        端到端基准 → bench/RESULTS.md
  micro.py          归因基准
  RESULTS.md        ★ 结论文档（含所有原始数据）
core/include/hxvm/  核心头文件（CPU 后端抽象、基础类型、日志）
cmake/toolchains/   iOS 交叉编译 toolchain
.github/workflows/  bench.yml（每次 push 重跑基准）+ ipa.yml（macOS 出 IPA）
scripts/            fetch-unicorn.sh
```

## 跑基准

```bash
pip install unicorn capstone numpy
python bench/micro.py       # 归因：读 vs 写 vs 纯寄存器
python bench/vmbench.py     # 端到端：链表/矩阵/混音/字符串
```

两份 C 源码都会被编译两次：一次 `-nostdlib -static` 成 ELF 给 Unicorn，
一次编成宿主原生 `.so` 直接调用，同样入参取墙钟比。

## CI

- **benchmark**：每次 push 都在 GitHub Actions 上重跑基准。这组数字是全部架构决策的依据，
  它们必须一直可被复现。
- **build-ipa**：macOS runner 上先交叉编译 **iOS 真机 arm64 的 Unicorn 静态库**（地基验证），
  再尝试打包 IPA。App 层（`platform/ios/`）提交前该步会跳过并报警。

  产出的是 **未签名 IPA** —— Actions 上没有 Apple 账号，
  装到设备上之前需要你自己重签（SideStore / AltStore / 开发者证书）。

---

## 状态

| 阶段 | 状态 |
|---|---|
| 可行性基准 + 归因 | ✅ 完成，结论明确 |
| CPU 后端抽象 / 构建地基 | 🟡 进行中 |
| PE 加载器 + IAT 打桩 | ⬜ 未开始 |
| Win32 垫片层 | ⬜ 未开始 |
| iOS 宿主（CADisplayLink / Metal / JIT W^X） | ⬜ 未开始 |

**已知且不会变的事实**：按当前（Unicorn）内核，成品是 2~6fps。要的东西如果是「能玩」，
在这个约束下拿不到；先决定要不要按上面第 1 条把内核换成直接映射 dynarec。
