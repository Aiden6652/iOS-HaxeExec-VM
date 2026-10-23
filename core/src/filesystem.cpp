// SPDX-License-Identifier: MIT
#include "hxvm/filesystem.hpp"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>

namespace hxvm {
namespace {

std::string Lower(const std::string &s) {
    std::string o = s;
    std::transform(o.begin(), o.end(), o.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return o;
}

std::string JoinPath(const std::string &a, const std::string &b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    return a + "/" + b;
}

std::vector<std::string> Split(const std::string &s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

void Walk(const std::string &root, const std::string &rel,
          std::unordered_map<std::string, std::string> &files,
          std::map<std::string, std::map<std::string, std::string>> &children,
          usize &count) {
    std::string dir = rel.empty() ? root : JoinPath(root, rel);
    DIR *d = opendir(dir.c_str());
    if (!d) return;

    while (struct dirent *e = readdir(d)) {
        std::string name = e->d_name;
        if (name == "." || name == "..") continue;

        std::string child_rel = JoinPath(rel, name);
        children[Lower(rel)][Lower(name)] = name;

        bool is_dir = false;
#if defined(DT_DIR)
        if (e->d_type == DT_DIR) is_dir = true;
        else if (e->d_type == DT_UNKNOWN) {
#endif
            struct stat st;
            if (stat(JoinPath(dir, name).c_str(), &st) == 0) is_dir = S_ISDIR(st.st_mode);
#if defined(DT_DIR)
        }
#endif
        if (is_dir) {
            Walk(root, child_rel, files, children, count);
        } else {
            files[Lower(child_rel)] = child_rel;
            count++;
        }
    }
    closedir(d);
}

}  // namespace

void FileSystem::SetRoot(const std::string &root) {
    root_ = root;
    while (!root_.empty() && root_.back() == '/') root_.pop_back();
}

void FileSystem::SetWorkDir(const std::string &wd) {
    workdir_ = Normalize(wd);
    while (!workdir_.empty() && workdir_.front() == '/') workdir_.erase(workdir_.begin());
}

usize FileSystem::BuildIndex() {
    files_.clear();
    children_.clear();
    children_[""];            // 根目录条目
    usize count = 0;
    Walk(root_, "", files_, children_, count);
    return count;
}

std::string FileSystem::Normalize(const std::string &win_path) const {
    std::string s;
    s.reserve(win_path.size());

    // 去盘符： "C:\a\b" → "\a\b"
    usize start = 0;
    if (win_path.size() >= 2 && win_path[1] == ':' &&
        ((win_path[0] | 0x20) >= 'a' && (win_path[0] | 0x20) <= 'z')) {
        start = 2;
    }

    for (usize i = start; i < win_path.size(); i++) {
        char c = win_path[i];
        if (c == '\\') c = '/';       // 唯一的路径分隔符差异
        s.push_back(c);
    }

    // 压掉重复的 "/" 和 "./"
    std::string out;
    for (usize i = 0; i < s.size();) {
        if (s[i] == '/') {
            out.push_back('/');
            while (i < s.size() && s[i] == '/') i++;
        } else if (s.compare(i, 2, "./") == 0 && (out.empty() || out.back() == '/')) {
            i += 2;
        } else {
            out.push_back(s[i]);
            i++;
        }
    }
    if (out == "./") out.clear();
    return out;
}

std::string FileSystem::Resolve(const std::string &win_path) const {
    std::string p = Normalize(win_path);
    if (p.empty()) {
        if (log_misses_) misses_.push_back(win_path);
        return "";
    }

    const bool absolute = p.front() == '/';
    std::string result = absolute ? "" : workdir_;

    for (const auto &seg : Split(p, '/')) {
        if (seg.empty() || seg == ".") continue;
        if (seg == "..") {
            usize pos = result.rfind('/');
            result = (pos == std::string::npos) ? "" : result.substr(0, pos);
            continue;
        }
        auto it = children_.find(Lower(result));
        if (it == children_.end()) {
            if (log_misses_) misses_.push_back(win_path);
            return "";
        }
        auto jt = it->second.find(Lower(seg));
        if (jt == it->second.end()) {
            if (log_misses_) misses_.push_back(win_path);
            return "";
        }
        result = JoinPath(result, jt->second);
    }

    return result.empty() ? root_ : JoinPath(root_, result);
}

std::vector<std::string> FileSystem::ListDir(const std::string &win_dir) const {
    std::vector<std::string> out;
    std::string p = Normalize(win_dir);
    if (!p.empty() && p.front() == '/') p.erase(p.begin());
    auto it = children_.find(Lower(p));
    if (it == children_.end()) return out;
    for (const auto &kv : it->second) out.push_back(kv.second);
    return out;
}

}  // namespace hxvm
