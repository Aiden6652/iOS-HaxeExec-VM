// iOS-HaxeExec-VM / benchmark workloads
//
// 设计约束：这些 workload 代表 Psych Engine(lime/openfl/hxcpp) 一帧里的典型代价，
// 全部是纯计算，不依赖 libc —— 用 -nostdlib 编译成 ELF 后可以被整体映射到 Unicorn
// 里执行（因为 .text/.rodata/.data 的 RIP 相对寻址在原 vaddr 上是自洽的）。
//
// 同一个文件再编译一次为 host 原生 .so，用来做 "原生 vs 模拟" 的对照基准。

#include <stdint.h>
#include <stddef.h>

typedef int64_t i64;
typedef uint64_t u64;

/* ------------------------------------------------------------------ */
/* 1) 指针追逐：模拟 Haxe GC 堆上对象图的遍历（cache miss 主导）        */
/* ------------------------------------------------------------------ */
u64 bench_ptrchase(u64 *arena, i64 n)
{
    u64 idx = 0, acc = 0;
    const u64 mask = (u64)n - 1ULL;
    for (i64 i = 0; i < n; i++) {
        idx = arena[idx & mask];
        acc += (idx ^ acc) + (u64)i;
    }
    return acc;
}

/* ------------------------------------------------------------------ */
/* 2) 对象字段遍历：模拟 openfl DisplayObject 链表逐帧 update/transform */
/* ------------------------------------------------------------------ */
typedef struct Node {
    struct Node *next;
    i64 x, y, scaleMode;
    double a, sx, sy, rot;
    i64 visible;
    char pad[48];
} Node;

struct NodePool {
    Node nodes[0];
};

u64 bench_objfields(Node *list, i64 iters)
{
    u64 acc = 0;
    for (i64 it = 0; it < iters; it++) {
        for (Node *o = list; o; o = o->next) {
            if (!o->visible) continue;
            o->x += 1;
            o->y += 2;
            o->rot += 0.0015;
            o->sx = o->sx * 1.0000001 + 0.0000001;
            o->sy = o->sy * 1.0000002 + 0.0000002;
            acc += (u64)(o->x ^ o->y) + (u64)o->rot;
        }
    }
    return acc;
}

/* 对照实验：每轮只碰 1 个字段 vs 碰 8 个字段。
   如果 slowdown 随「每次迭代的访存次数」线性放大，就证明瓶颈是 Unicorn 的
   soft-MMU 访存路径（每条访存指令都要走 helper + TLB 查表），而不是翻译本身。 */
u64 bench_fields1(Node *list, i64 iters)
{
    u64 acc = 0;
    for (i64 it = 0; it < iters; it++)
        for (Node *o = list; o; o = o->next)
            acc += (u64)o->x;
    return acc;
}

u64 bench_fields8(Node *list, i64 iters)
{
    u64 acc = 0;
    for (i64 it = 0; it < iters; it++) {
        for (Node *o = list; o; o = o->next) {
            o->x += 1; o->y += 2; o->visible += 3; o->scaleMode += 4;
            o->a += 0.0015; o->sx *= 1.0000001; o->sy *= 1.0000002; o->rot += 0.001;
            acc += (u64)(o->x ^ o->y ^ o->visible ^ o->scaleMode);
        }
    }
    return acc;
}

/* ------------------------------------------------------------------ */
/* 3) 字符串：hxcpp 在 Windows 上是 UTF-16 的 hx::String，           */
/*    资源 key 查找 / 谱面解析重度依赖字符串 hash + 比较              */
/* ------------------------------------------------------------------ */
static u64 hash_u16(const uint16_t *s, i64 len)
{
    u64 h = 1469598103934665603ULL;      // FNV-1a (|1<<64| 常量会进 .rodata，没问题)
    for (i64 i = 0; i < len; i++) {
        h ^= (u64)s[i];
        h *= 1099511628211ULL;
    }
    return h;
}

u64 bench_strings(uint16_t *buf, i64 words, i64 reps)
{
    u64 acc = 0;
    for (i64 r = 0; r < reps; r++) {
        acc += hash_u16(buf + (r & 15), words);
    }
    return acc;
}

/* ------------------------------------------------------------------ */
/* 4) 4x4 矩阵：精灵批处理里每个 sprite 的 transform 合成              */
/* ------------------------------------------------------------------ */
typedef double M44[4][4];

static void mul4(const M44 a, const M44 b, M44 out)
{
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            double s = 0.0;
            for (int k = 0; k < 4; k++) s += a[i][k] * b[k][j];
            out[i][j] = s;
        }
    }
}

u64 bench_matrix(M44 *pool, i64 count, i64 reps)
{
    u64 acc = 0;
    for (i64 r = 0; r < reps; r++) {
        for (i64 i = 0; i < count; i++) {
            M44 tmp;
            mul4(pool[i], pool[0], tmp);
            acc += (u64)(tmp[0][0] * 1000.0);
        }
    }
    return acc;
}

/* ------------------------------------------------------------------ */
/* 5) PCM 混音：SDL audio callback，32 路 int16 混合               */
/* ------------------------------------------------------------------ */
u64 bench_pcmix(int16_t *chans, i64 frames, i64 nchans)
{
    u64 acc = 0;
    for (i64 f = 0; f < frames; f++) {
        int32_t sum = 0;
        for (i64 c = 0; c < nchans; c++) sum += chans[c * frames + f];
        sum >>= 2;
        if (sum > 32767) sum = 32767;
        if (sum < -32768) sum = -32768;
        acc += (u64)(int16_t)sum;
    }
    return acc;
}

/* ------------------------------------------------------------------ */
/* 6) 合成一帧：按经验比例把上面几件事揉在一起                        */
/*    (比例来自 Psych Engine update+render 的粗略画像)                */
/* ------------------------------------------------------------------ */
u64 bench_frame(u64 *arena, Node *list, M44 *pool, i64 spriteCount)
{
    u64 acc = 0;
    /* update(): 对象遍历 .40 */
    acc += bench_objfields(list, 1);
    /* update(): 字符串/资源查找 .10 */
    acc += bench_strings((uint16_t *)arena, 256, 64);
    /* render(): transform 合成 .30 */
    acc += bench_matrix(pool, spriteCount, 1);
    /* render(): 输入/判定用的少量指针追逐 .10 */
    acc += bench_ptrchase(arena, 4096);
    return acc;
}

/* ------------------------------------------------------------------ */
/* 7) VM 退出代价探针：循环 N 次 syscall(SYSCALL 指令)                */
/*    用来量化 "每帧最多能承受多少次导入函数调用"                      */
/* ------------------------------------------------------------------ */
static inline long sys_probe(long a, long b)
{
    long ret;
    __asm__ volatile("mov %1, %%rax\n\t"
                     "mov %2, %%rdi\n\t"
                     "syscall\n\t"
                     "mov %%rax, %0"
                     : "=r"(ret)
                     : "r"(a), "r"(b)
                     : "rax", "rdi", "rcx", "r11");
    return ret;
}

u64 bench_exitloop(i64 n)
{
    u64 acc = 0;
    for (i64 i = 0; i < n; i++) {
        /* 每次 syscall 都会导致一次完整的 VM exit + C 回调 */
        acc += (u64)sys_probe(39, 0); /* getpid */
    }
    return acc;
}

/* -nostdlib 需要一个入口符号，避免链接器警告/默认入口错误 */
void _start(void)
{
    for (;;) { }
}
