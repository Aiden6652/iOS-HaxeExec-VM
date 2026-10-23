// SPDX-License-Identifier: MIT
// 文件系统桥接 —— 这个项目里最容易让人卡死的一处。
//
// Windows：路径分隔符 `\`，且**不区分大小写**。
// iOS：分隔符 `/`，且**区分大小写**。
// 原 EXE 里写死的路径（assets\songs\Week1\icon.png）在 iOS 上必然落空，
// 不拦下来资源加载 100% 卡死。
//
// 做法：启动时把资源根遍历一遍建索引（路径逐段小写 → 真实名字），
// 之后每次解析是 O(段数) 的查表，不在每次 open 时去扫盘。
#pragma once

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "hxvm/basics.hpp"

namespace hxvm {

class FileSystem {
public:
    // root: iOS 沙盒里游戏资源的根（比如 Documents/Game）
    void SetRoot(const std::string &root);
    // 来宾视角的当前目录（相对 root，用 `/`）
    void SetWorkDir(const std::string &wd);

    // 建索引。资源量大时会花点时间，只在启动时做一次。
    // 返回索引到的文件数。
    usize BuildIndex();

    // Windows 路径 → 宿主真实路径。找不到返回空串。
    // 会自动处理：`\`→`/`、盘符、大小写、路径穿越、末尾多余分隔符。
    std::string Resolve(const std::string &win_path) const;

    // 在给定目录下按名字（大小写不敏感）列目录项，供 FindFirstFile 用
    std::vector<std::string> ListDir(const std::string &win_dir) const;

    const std::string &root() const { return root_; }
    const std::string &workdir() const { return workdir_; }

    // 记录所有解析失败的路径，方便事后一次性看出缺了哪些资源
    void EnableMissLog(bool on) { log_misses_ = on; }
    const std::vector<std::string> &misses() const { return misses_; }

private:
    std::string Normalize(const std::string &win_path) const;

    std::string root_;
    std::string workdir_;
    // 小写相对路径 → 真实相对路径
    std::unordered_map<std::string, std::string> files_;
    // 小写相对目录 → (小写名字 → 真实名字)
    std::map<std::string, std::map<std::string, std::string>> children_;
    bool log_misses_ = false;
    mutable std::vector<std::string> misses_;
};

}  // namespace hxvm
