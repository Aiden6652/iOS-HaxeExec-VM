// 受控微基准：拆出一个字段访问循环的代价来源。
// variant 由 micro.py 传入，所有分支都在同一份数据上跑同样的迭代次数，
// 这样 native / guest 两侧的比值就能直接归因。

#include <stdint.h>

typedef int64_t i64;
typedef uint64_t u64;

typedef struct Node {
    struct Node *next;
    i64 f[8];
    double d[8];
    i64 visible;
} Node;

// 0: 1 个 int64 读
// 1: 4 个 int64 读
// 2: 8 个 int64 读
// 3: 4 个 int64 写
// 4: 4 个 int64 读 + 4 个 int64 写
// 5: 4 个 double 读
// 6: 4 个 double 写
// 7: 4 个 double 读 + 4 个 double 写
// 8: 纯寄存器算术（完全不访存，测翻译器纯计算开销）
u64 micro(int variant, Node *list, i64 iters)
{
    u64 acc = 0;
    switch (variant) {
    case 0:
        for (i64 it = 0; it < iters; it++)
            for (Node *o = list; o; o = o->next) acc += (u64)o->f[0];
        break;
    case 1:
        for (i64 it = 0; it < iters; it++)
            for (Node *o = list; o; o = o->next)
                acc += (u64)(o->f[0] + o->f[1] + o->f[2] + o->f[3]);
        break;
    case 2:
        for (i64 it = 0; it < iters; it++)
            for (Node *o = list; o; o = o->next)
                acc += (u64)(o->f[0] + o->f[1] + o->f[2] + o->f[3] +
                             o->f[4] + o->f[5] + o->f[6] + o->f[7]);
        break;
    case 3:
        for (i64 it = 0; it < iters; it++)
            for (Node *o = list; o; o = o->next) {
                o->f[0] += 1; o->f[1] += 2; o->f[2] += 3; o->f[3] += 4;
                acc += (u64)o->f[0];
            }
        break;
    case 4:
        for (i64 it = 0; it < iters; it++)
            for (Node *o = list; o; o = o->next) {
                o->f[0] = o->f[1] + 1; o->f[1] = o->f[2] + 1;
                o->f[2] = o->f[3] + 1; o->f[3] = o->f[0] + 1;
                acc += (u64)o->f[3];
            }
        break;
    case 5:
        for (i64 it = 0; it < iters; it++)
            for (Node *o = list; o; o = o->next)
                acc += (u64)(o->d[0] + o->d[1] + o->d[2] + o->d[3]);
        break;
    case 6:
        for (i64 it = 0; it < iters; it++)
            for (Node *o = list; o; o = o->next) {
                o->d[0] += 0.001; o->d[1] += 0.002;
                o->d[2] += 0.003; o->d[3] += 0.004;
                acc += (u64)o->d[0];
            }
        break;
    case 7:
        for (i64 it = 0; it < iters; it++)
            for (Node *o = list; o; o = o->next) {
                o->d[0] = o->d[1] * 1.0000001; o->d[1] = o->d[2] * 1.0000001;
                o->d[2] = o->d[3] * 1.0000001; o->d[3] = o->d[0] * 1.0000001;
                acc += (u64)o->d[3];
            }
        break;
    case 8:
        for (i64 it = 0; it < iters; it++) {
            u64 a = acc, b = it, c = a ^ b;
            for (int k = 0; k < 8; k++) { a += b ^ c; b += c ^ a; c += a ^ b; }
            acc = a ^ b ^ c;
        }
        break;
    }
    return acc;
}

void _start(void) { for (;;) { } }
