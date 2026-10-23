// SPDX-License-Identifier: MIT
// hxvm-cli：在开发机上直接跑一个 x86_64 Windows EXE，用来验证整条链路。
//
//   hxvm-cli <game.exe> [资源根] [--instr=N] [--quiet]
//
// 它和 iOS 宿主用的是同一套 core，区别只是没有渲染/音频后端和 CADisplayLink。
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "hxvm/log.hpp"
#include "hxvm/process.hpp"

using hxvm::usize;

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "用法：hxvm-cli <game.exe> [资源根] [--instr=N] [--quiet]\n");
        return 2;
    }

    std::string exe = argv[1];
    std::string root = ".";
    std::vector<std::string> guest_argv;
    usize instr_budget = 0;
    bool quiet = false;
    bool trace = false;

    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        if (a.rfind("--instr=", 0) == 0) {
            instr_budget = strtoull(a.substr(8).c_str(), nullptr, 10);
        } else if (a == "--quiet") {
            quiet = true;
        } else if (a == "--trace") {
            trace = true;
        } else if (!a.empty() && a[0] == '-') {
            // 忽略其它开关
        } else if (root == ".") {
            root = a;
        } else {
            guest_argv.push_back(a);
        }
    }

    if (quiet) hxvm::log::set_level(hxvm::log::Level::Warn);

    fprintf(stderr, "[hxvm] 来宾映像：%s\n", exe.c_str());
    fprintf(stderr, "[hxvm] 资源根　：%s\n", root.c_str());

    hxvm::Process proc;
    if (!proc.Init(exe, root, guest_argv)) {
        fprintf(stderr, "[hxvm] 加载失败\n");
        return 1;
    }

    if (trace) proc.EnableInstructionTrace(true);
    const int code = proc.Run(instr_budget);

    // ── 运行后体检 ──────────────────────────────────────────────────
    fprintf(stderr, "\n──────── 结果 ────────\n");
    fprintf(stderr, "退出码：%d\n", code);

    const auto &missing = proc.shims().missing();
    if (!missing.empty()) {
        fprintf(stderr, "\n没有实现、被直接返回 0 的导入（%zu 个）：\n", missing.size());
        for (const auto &m : missing) fprintf(stderr, "  - %s\n", m.c_str());
    }

    const auto &misses = proc.fs().misses();
    if (!misses.empty()) {
        usize uniq = 0;
        std::vector<std::string> seen;
        for (const auto &m : misses) {
            bool dup = false;
            for (const auto &s : seen) if (s == m) { dup = true; break; }
            if (!dup) { seen.push_back(m); uniq++; }
        }
        fprintf(stderr, "\n解析不到的路径（去重后 %zu 条，共 %zu 次）：\n", uniq, misses.size());
        for (usize i = 0; i < seen.size() && i < 40; i++) {
            fprintf(stderr, "  - %s\n", seen[i].c_str());
        }
        if (seen.size() > 40) fprintf(stderr, "  ... 还有 %zu 条\n", seen.size() - 40);
    }

    return code;
}
