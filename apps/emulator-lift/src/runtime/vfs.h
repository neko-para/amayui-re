// emulator-lift · VFS：Win32 guest 路径 ↔ 宿主路径（唯一根 = `--game-dir`；大小写不敏感）
//
// ★ 口径（plan §6 S6 / §9 R2）：
//   · `--game-dir` 是**唯一的根**。guest 的绝对路径先剥掉盘符与 guest 根前缀，再拼到 game_dir；
//     `..` 不许逃出根（逃出 = 响亮失败，⛔ 不静默夹回根里）。
//   · **大小写不敏感**：逐段在宿主目录里找唯一的大小写不敏感匹配；同一段有多个候选 ⇒ 报歧义
//     （Windows 上不会发生；Linux/macOS 上会发生 ⇒ 必须显式失败，不能按目录顺序猜）。
//   · 枚举顺序**排序后返回**（plan §8.3-4：`FindFirstFileA` 的顺序必须确定性）。
//   · 通配符只实现游戏实际用到的形态：`*` / `?` / `*.*`（Windows 语义：`*.*` 匹配全部）。
//     其它非法形态（`**`、`\` 后无名字等）⇒ 报错，⛔ 不静默忽略。
//   · 宿主路径一律用 **UTF-8** 字符串（Windows 上经 `std::filesystem::u8path`）。
// ★ 这是**叶子设施**：不做缓存、不碰 guest 内存；shim 负责读写与错误码。
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace lift {

struct HostDirEntry {
    std::string name;      // 宿主目录里的**真实**文件名（UTF-8）
    bool is_dir = false;
    bool is_file = false;
    std::uint64_t size = 0;
    std::uint32_t attributes = 0;  // FILE_ATTRIBUTE_* 的跨平台近似
    // 三个宿主时间（自 1601-01-01 起的 100ns = Windows FILETIME）；取不到时回退成 write、
    // 或按文档留 0（"文件系统不支持"）。
    std::uint64_t create_time_100ns = 0;
    std::uint64_t access_time_100ns = 0;
    std::uint64_t write_time_100ns = 0;
};

// 一个宿主路径的三个时间（FILETIME 100ns）。
struct HostFileTimes {
    std::uint64_t create_100ns = 0;
    std::uint64_t access_100ns = 0;
    std::uint64_t write_100ns = 0;
};

// 取宿主文件的三个时间。★ 平台抽象走 **SDL3 的 `SDL_GetPathInfo`**（Windows / APFS / Linux 都实现，
// 含创建时间与访问时间；`std::filesystem` 只有 mtime），实现里**不写 `#ifdef` 平台分支**。
// 失败（路径不存在 / SDL 不可用）⇒ false，调用方回退到 `std::filesystem::last_write_time`。
bool hostFileTimes(const std::string& host_path, HostFileTimes* out);

// 按 **UTF-8 宿主路径**打开文件（Windows 上走 `_wfopen_s`）。
// ★ 为什么不能直接用 `std::fopen`：MSVC 的窄 `fopen` 按 **ANSI 代码页**解释路径，而本仓路径一律
//   UTF-8（游戏目录本身是日文名）⇒ Windows 上非 ASCII 的 `--exe` / `--lst` / `--game-dir` 会打不开。
std::FILE* openHostFileUtf8(const std::string& host_path, const char* mode);

class Vfs {
public:
    // 宿主根（绝对路径，UTF-8）。空 ⇒ ready() = false。
    void setGameDir(const std::string& host_dir);
    const std::string& gameDir() const { return game_dir_; }
    // ★ P0：伪造 AppData（--appdata）。只影响特殊目录（AppData / LocalAppData / Personal），
    //   不改变 --game-dir 是唯一游戏根的口径。空 = 未设置（特殊目录解析会响亮失败）。
    void setAppDataDir(const std::string& host_dir) { app_data_dir_ = host_dir; }
    const std::string& appDataDir() const { return app_data_dir_; }
    bool ready() const { return !game_dir_.empty(); }

    // guest 根的文本形式（缺省 `E:\Games\Eushully\天結`；只用于把路径的公共前缀剥掉与日志）
    void setGuestRoot(const std::string& guest_root) { guest_root_ = guest_root; }
    const std::string& guestRoot() const { return guest_root_; }

    // guest 当前目录（绝对 guest 路径；缺省 = guestRoot）
    void setGuestCwd(const std::string& guest_path);
    const std::string& guestCwd() const { return guest_cwd_; }

    // 解析：guest 路径 → 宿主绝对路径。失败 ⇒ false，error 写清"哪一段 / 为什么"。
    // `must_exist` = false 时最后一段可以不存在（CreateFile(CREATE_NEW) / 写文件）。
    // `resolve_case` = true 时逐段做大小写不敏感匹配；false 时最后一段允许保留调用方大小写
    // （用于创建新文件：父目录仍然要做大小写匹配）。
    bool resolve(const std::string& guest_path, std::string* host_path, std::string* error,
                 bool must_exist = false, bool resolve_case = true) const;

    // 反解：宿主路径 → guest 路径（用于 GetCurrentDirectory / GetModuleFileName；
    // 不在根下 ⇒ false）。
    bool toGuest(const std::string& host_path, std::string* guest_path) const;

    // 枚举 guest 目录下匹配通配符的条目（排序；`.` / `..` 不返回）。
    // `pattern` 是 guest 路径（可含 `*` / `?`）；目录部分必须存在。
    bool find(const std::string& guest_pattern, std::vector<HostDirEntry>* out, std::string* error) const;

    // 目录是否存在 / 文件是否存在（大小写不敏感）
    bool isDirectory(const std::string& guest_path) const;
    bool isFile(const std::string& guest_path) const;

    // ★ P4：`imagehlp!MakeSureDirectoryPathExists` 的落点（递归建目录）。
    //   口径（照 Win32 文档 + 引擎的实际用法，见 imagehlp.cpp 的文件头）：
    //   · 路径里**每个分隔符（`\` / `/`）之前**的段都是目录 ⇒ 必须存在；
    //   · **末尾分隔符**把最后一段也标成目录（引擎就是这么调的：`.lst` 0x00490383 先补 `\`）；
    //   · **没有**末尾分隔符时最后一段按**文件名**处理，⛔ 不建它；
    //   · 连续分隔符（`\\`）没有段 ⇒ 跳过；盘符 / guest 根 / `--appdata` 特殊目录这些"根"不创建
    //     （它们本来就在，⛔ 不许为 `E:\Games` 这种根前缀在宿主上造出一棵假目录树）；
    //   · **已存在算成功**（这是它与 `CreateDirectoryA` 的关键区别）。
    //   返回 false 时 `error` 写清是哪一段 / 为什么（调用方映射成 Win32 失败值）。
    bool makeSureDirectoryPathExists(const std::string& guest_path, std::string* error) const;

    // 常用：把 guest 路径规范化成"绝对 guest 路径"（不改大小写；用于日志 / 当前目录）
    std::string absoluteGuest(const std::string& guest_path) const;
    // ★ `GetFullPathNameA` 用：**纯文本**规范化（相对路径相对 guest cwd；折叠 `.` / `..`；
    //   `..` 到根即止；隔离符统一成 `\`），⛔ 不查宿主盘、不做大小写解析 —— Windows 的
    //   GetFullPathName 也不做（文档原话："does not verify that the resulting path ... sees an
    //   existing file"）。空串 ⇒ 返回空串（调用方映射成 ERROR_INVALID_PARAMETER）。
    std::string canonicalGuest(const std::string& guest_path) const;
    // 取父目录（guest 路径）；没有则空串
    static std::string parentGuest(const std::string& guest_path);
    static std::string baseName(const std::string& guest_path);

private:
    std::string game_dir_;
    std::string app_data_dir_;
    std::string guest_root_ = "E:\\Games\\Eushully\\天結";
    std::string guest_cwd_;
    bool guest_cwd_set_ = false;
};

// 跨平台 FILE_ATTRIBUTE_*（只实现本线用到的几个）
constexpr std::uint32_t kAttrReadOnly = 0x00000001u;
constexpr std::uint32_t kAttrDirectory = 0x00000010u;
constexpr std::uint32_t kAttrArchive = 0x00000020u;
constexpr std::uint32_t kAttrNormal = 0x00000080u;

}  // namespace lift
