// SPDX-License-Identifier: MIT
#include "hxvm/registry.hpp"

#include <algorithm>

namespace hxvm {
namespace {

std::string Lower(const std::string &s) {
    std::string o = s;
    std::transform(o.begin(), o.end(), o.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return o;
}

std::vector<std::string> Split(const std::string &s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep || c == '\\') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

}  // namespace

Registry::Registry() {
    SeedDefaults();
}

std::string Registry::Canonicalize(const std::string &path) const {
    // 缩写根键
    static const std::pair<std::string, std::string> kAlias[] = {
        {"hklm", "hkey_local_machine"},
        {"hkcu", "hkey_current_user"},
        {"hkcr", "hkey_classes_root"},
        {"hku", "hkey_users"},
        {"hkcc", "hkey_current_config"},
    };
    std::string p = Lower(path);
    for (const auto &kv : kAlias) {
        if (p.compare(0, kv.first.size(), kv.first) == 0) {
            p = kv.second + p.substr(kv.first.size());
            break;
        }
    }
    return p;
}

u64 Registry::OpenKey(const std::string &path, bool create) {
    const std::string key = Canonicalize(path);
    if (keys_.find(key) == keys_.end()) {
        if (!create) return 0;
        keys_[key];            // 建空键
    }
    const u64 h = ++next_;
    open_key_paths_[h] = key;
    return h;
}

void Registry::CloseKey(u64 key) { open_key_paths_.erase(key); }

bool Registry::IsKey(u64 key) const {
    return open_key_paths_.find(key) != open_key_paths_.end();
}

std::string Registry::KeyPath(u64 handle) const {
    auto it = open_key_paths_.find(handle);
    return it == open_key_paths_.end() ? std::string() : it->second;
}

// 注：本实现里句柄值与键路径一一对应，靠顺序还原。
// 这么写是为了保持状态简单 —— 注册表在这个项目里的用途只是「别让程序拿到空值」。
bool Registry::QueryValue(u64 key, const std::string &name, u32 &type,
                          std::vector<u8> &out) const {
    auto hit = open_key_paths_.find(key);
    if (hit == open_key_paths_.end()) return false;

    auto kit = keys_.find(hit->second);
    if (kit == keys_.end()) return false;

    auto vit = kit->second.find(Lower(name));
    if (vit == kit->second.end()) vit = kit->second.find("");   // 默认值
    if (vit == kit->second.end()) return false;

    type = vit->second.type;
    out = vit->second.data;
    return true;
}

bool Registry::SetValue(u64 key, const std::string &name, u32 type,
                        const void *data, usize size) {
    auto hit = open_key_paths_.find(key);
    if (hit == open_key_paths_.end()) return false;

    RegValue v;
    v.type = type;
    const u8 *p = static_cast<const u8 *>(data);
    v.data.assign(p, p + size);
    keys_[hit->second][Lower(name)] = v;
    return true;
}

void Registry::SeedDefaults() {
    auto put_sz = [this](const std::string &key, const std::string &name,
                         const std::string &val) {
        keys_[Lower(key)][Lower(name)] = {kRegSz,
                                         std::vector<u8>(val.begin(), val.end())};
    };
    auto put_dw = [this](const std::string &key, const std::string &name, u32 val) {
        std::vector<u8> b(4);
        for (int i = 0; i < 4; i++) b[i] = static_cast<u8>((val >> (8 * i)) & 0xFF);
        keys_[Lower(key)][Lower(name)] = {kRegDword, b};
    };

    const std::string nt = "HKEY_LOCAL_MACHINE\\Software\\Microsoft\\Windows NT\\CurrentVersion";
    put_sz(nt, "ProductName", "Windows 10 Pro");
    put_sz(nt, "CurrentBuildNumber", "19045");
    put_sz(nt, "CurrentVersion", "10.0");
    put_sz(nt, "ProductId", "00330-80000-00000-AA123");
    put_dw(nt, "InstallDate", 0x5F5E1000);

    const std::string intl = "HKEY_CURRENT_USER\\Control Panel\\International";
    put_sz(intl, "LocaleName", "en-US");
    put_sz(intl, "sLanguage", "ENU");

    const std::string desk = "HKEY_CURRENT_USER\\Control Panel\\Desktop";
    put_dw(desk, "LogPixels", 96);
    put_sz(desk, "Wallpaper", "");

    const std::string mui = "HKEY_CURRENT_USER\\Control Panel\\Desktop\\MuiCached";
    put_sz(mui, "MachinePreferredUILanguages", "en-US");

    // 不少程序会先探一下 .NET / VC runtime 在不在
    const std::string fx = "HKEY_LOCAL_MACHINE\\Software\\Microsoft\\NET Framework Setup\\NDP\\v4\\Full";
    put_dw(fx, "Install", 1);
    put_dw(fx, "Release", 528040);
}

}  // namespace hxvm
