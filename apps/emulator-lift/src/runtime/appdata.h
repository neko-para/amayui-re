// emulator-lift · **`ctx.appdata` 的结果值类型**（插件宿主 I/O；语言无关）
//
// 合同：`docs/03-emulator/plugin-runtime.md` §4.5（Context）/ §4.6（JS 契约）/ §6 行 38。
//
// ★ 为什么是**结果对象**而不是异常：`stat` 的"不存在"是**正常结果**（`loadSave` 的前置检查就靠它），
//   路径穿越 / 非法输入也是插件自己的编程错误 —— 走 `error` 码，⛔ 不把 `errno` / 平台文案
//   漏给 JS（那会让同一份插件在不同宿主上看到不同的字符串）。
// ★ 错误码是**稳定词表**（d.ts 的 `AppDataStatError` 与这里逐字一致）；新增码 = 改合同。
#pragma once

#include <cstdint>

namespace lift::plugin {

enum class AppDataStatError : std::uint8_t {
    None = 0,
    NoAppData = 1,    // 没有 `--appdata` / 根不可用（`available=false`）
    InvalidPath = 2,  // 空串 / 绝对路径 / `..` 段 / NUL 或控制字符
    Escape = 3,       // canonical 之后不在 appdata 根下（符号链接 / junction 逃逸）
    NotFound = 4,     // 目标不存在（含父目录不存在）
    NotDir = 5,       // 路径中间有一段是文件 / 不是目录
    Denied = 6,       // 权限拒绝
    Io = 7,           // 其它宿主 I/O 错误
};

// 给 JS 的名字（`AppDataStat.error`）；⛔ 不要在这里透出 `std::error_code` 的 message。
inline const char* appDataStatErrorName(AppDataStatError error) {
    switch (error) {
        case AppDataStatError::None:
            return "";
        case AppDataStatError::NoAppData:
            return "no_appdata";
        case AppDataStatError::InvalidPath:
            return "invalid_path";
        case AppDataStatError::Escape:
            return "escape";
        case AppDataStatError::NotFound:
            return "not_found";
        case AppDataStatError::NotDir:
            return "not_dir";
        case AppDataStatError::Denied:
            return "denied";
        case AppDataStatError::Io:
            return "io";
    }
    return "io";
}

struct AppDataStat {
    bool exists = false;
    bool is_file = false;
    bool is_dir = false;
    bool has_size = false;  // 只有 is_file 且 `file_size` 成功时给 `size`
    std::uint64_t size = 0;
    AppDataStatError error = AppDataStatError::None;
};

}  // namespace lift::plugin
