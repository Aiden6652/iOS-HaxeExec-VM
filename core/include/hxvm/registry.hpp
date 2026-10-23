// SPDX-License-Identifier: MIT
// 内存里伪造的注册表。原 EXE 读的那些 Windows 键在 iOS 上不存在，
// 与其让它拿到「键不存在」然后走奇怪的分支，不如给一套合理的默认值。
#pragma once

#include <map>
#include <unordered_map>
#include <string>
#include <vector>

#include "hxvm/basics.hpp"

namespace hxvm {

// REG_* 类型
enum RegType {
    kRegNone = 0,
    kRegSz = 1,
    kRegExpandSz = 2,
    kRegBinary = 3,
    kRegDword = 4,
    kRegDwordBigEndian = 5,
    kRegLink = 6,
    kRegMultiSz = 7,
    kRegQword = 11,
};

// 根键句柄（Windows 的预定义值）
enum RegPredefinedKey {
    kHkeyClassesRoot = 0x80000000,
    kHkeyCurrentUser = 0x80000001,
    kHkeyLocalMachine = 0x80000002,
    kHkeyUsers = 0x80000003,
    kHkeyPerformanceData = 0x80000004,
    kHkeyCurrentConfig = 0x80000005,
};

struct RegValue {
    u32 type = kRegNone;
    std::vector<u8> data;
};

class Registry {
public:
    Registry();

    // "HKEY_LOCAL_MACHINE\Software\X" 或 "HKLM\Software\X"
    // 返回句柄值（0 = 失败）
    u64 OpenKey(const std::string &path, bool create);
    void CloseKey(u64 key);
    bool IsKey(u64 key) const;

    // 返回 false = 键或值不存在
    bool QueryValue(u64 key, const std::string &name, u32 &type,
                    std::vector<u8> &out) const;
    bool SetValue(u64 key, const std::string &name, u32 type,
                  const void *data, usize size);

    // 预填一批 Windows 上必然存在、且程序常读的键
    void SeedDefaults();

    // 给定句柄，拿回它的完整路径（嵌套打开子键时要靠它拼路径）
    std::string KeyPath(u64 handle) const;

private:
    std::string Canonicalize(const std::string &path) const;
    std::map<std::string, std::map<std::string, RegValue>> keys_;  // 全小写
    // 句柄必须直接映射到路径。早先按「打开顺序的下标」去 keys_（字母序）里取第 N 个，
    // 取到的根本不是同一个键 —— 这是 RegQueryValueExA 永远查不到值的根因。
    std::unordered_map<u64, std::string> open_key_paths_;
    u64 next_ = 0;
};

}  // namespace hxvm
