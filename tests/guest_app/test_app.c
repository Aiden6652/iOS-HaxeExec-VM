// 端到端验证用的来宾程序：用 mingw 编成真正的 x86_64 Windows EXE，
// 再交给 hxvm-cli 在 Unicorn 里跑。
//
// 它刻意用「Windows 风格」的路径去请求一个「iOS 风格」存储的文件：
//   请求 assets\Songs\WEEK1\Icon.PNG
//   实际 assets/songs/week1/icon.png
// 大小写和分隔符都故意对不上 —— 这正是资源加载最容易卡死的地方。
//
// 链接时要 -mconsole，让 CRT 走控制台输出路径（会经 WriteFile）。

#include <windows.h>
#include <stdio.h>

int main(void) {
    printf("=== 来宾程序启动 ===\n");

    /* 1) 文件：大小写混乱 + 反斜杠 */
    HANDLE h = CreateFileA("assets\\Songs\\WEEK1\\Icon.PNG",
                           GENERIC_READ, 0, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        printf("  [FAIL] 打不开 Icon.PNG（LastError=%lu）\n", GetLastError());
    } else {
        DWORD size = GetFileSize(h, NULL);
        char buf[128];
        DWORD read = 0;
        ReadFile(h, buf, sizeof(buf) - 1, &read, NULL);
        buf[read >= sizeof(buf) ? sizeof(buf) - 1 : read] = '\0';
        printf("  [OK] 打开 Icon.PNG，大小 %lu，读到 %lu 字节：%s\n",
               size, read, buf);
        CloseHandle(h);
    }

    /* 2) 换个大小写再开一次，验证是解析层在兜底而不是碰巧 */
    h = CreateFileA("ASSETS/songs/week1/ICON.png",
                    GENERIC_READ, 0, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    printf("  [%s] 全大写路径 %s\n",
           h != INVALID_HANDLE_VALUE ? "OK" : "FAIL", "ASSETS/songs/week1/ICON.png");
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);

    /* 3) 一个不存在的路径，应该干净地失败而不是卡住 */
    h = CreateFileA("assets\\nope\\missing.png",
                    GENERIC_READ, 0, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    printf("  [%s] 缺失文件按预期失败\n",
           h == INVALID_HANDLE_VALUE ? "OK" : "FAIL");

    /* 4) 堆 */
    HANDLE heap = GetProcessHeap();
    if (heap) {
        void *p = HeapAlloc(heap, HEAP_ZERO_MEMORY, 256);
        printf("  [%s] HeapAlloc -> %s\n", p ? "OK" : "FAIL", p ? "非 0" : "NULL");
        if (p) { HeapSize(heap, 0, p); HeapFree(heap, 0, p); }
    }

    /* 5) 高精度计时 */
    LARGE_INTEGER pc, freq;
    QueryPerformanceCounter(&pc);
    QueryPerformanceFrequency(&freq);
    printf("  [OK] QPC=%lld 频率=%lld\n", (long long)pc.QuadPart,
           (long long)freq.QuadPart);
    printf("  [OK] TickCount=%lu\n", GetTickCount());

    /* 6) 注册表 */
    HKEY key;
    LONG r = RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                           "Software\\Microsoft\\Windows NT\\CurrentVersion",
                           0, KEY_READ, &key);
    if (r == ERROR_SUCCESS) {
        char val[128];
        DWORD vsize = sizeof(val);
        DWORD vtype = 0;
        if (RegQueryValueExA(key, "ProductName", NULL, &vtype,
                             (LPBYTE)val, &vsize) == ERROR_SUCCESS) {
            printf("  [OK] 注册表 ProductName=%s\n", val);
        }
        RegCloseKey(key);
    } else {
        printf("  [FAIL] RegOpenKeyExA 返回 %ld\n", r);
    }

    /* 7) 目录枚举 */
    WIN32_FIND_DATAA fd;
    HANDLE find = FindFirstFileA("assets\\songs\\*", &fd);
    if (find != INVALID_HANDLE_VALUE) {
        printf("  [OK] FindFirstFile: %s\n", fd.cFileName);
        int n = 0;
        while (FindNextFileA(find, &fd) && n < 8) { n++; }
        printf("  [OK] FindNextFile 又枚举了 %d 项\n", n);
        FindClose(find);
    } else {
        printf("  [FAIL] FindFirstFile\n");
    }

    /* 8) 弹窗（会被转成日志，不会真弹） */
    MessageBoxA(NULL, "这条应该出现在日志里而不是屏幕上", "来宾 MessageBox", MB_OK);

    /* 9) 命令行 */
    printf("  [OK] 命令行=%s\n", GetCommandLineA());
    printf("  [OK] 模块名=%s\n", GetModuleFileNameA(NULL, NULL, 0) ? "" : "");

    printf("=== 来宾程序结束，返回 42 ===\n");
    return 42;
}
