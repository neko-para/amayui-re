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

// =======================================================================================
// ★ guest 特殊目录 / 环境块 —— **唯一文本真源**（2026-11-05 用户裁决）
//
//   环境块（`setupProcessStructures` 写进 guest 的那一块）、`GetEnvironmentVariableA/W`、
//   `GetEnvironmentStringsA/W`、`SHGetSpecialFolderPathA` / `SHGetFolderPathA` 全部从这一处取。
//   ⛔ 不许在 shim 里再写一份路径文本 —— 两套各说各话正是"引擎拿不到 LOCALAPPDATA、回退到
//   安装目录"那个 bug 的成因。
//
//   · guest 文本固定为 Windows 形状（`C:\Users\Amayui\...`），由 Vfs::resolve 映射：
//     `…\AppData\Local` → `<--appdata>/Local`、`…\AppData\Roaming` → `<--appdata>/Roaming`、
//     `C:\Users\Amayui\Documents` → `<--appdata>/Documents`；其余（USERPROFILE 本身）留在
//     `--game-dir` 的单一根下（⚠ 不会自动存在：那是 guest 世界的文本，不是宿主目录树）。
//   · **没有 `--appdata` 时，AppData 派生的变量一律不提供**（与 `SHGetSpecialFolderPathA` 的
//     FALSE + 5 一致；⛔ 绝不指向宿主真实 AppData）。理由：两个视图说同一件事比"env 有一个
//     解析时必然失败的路径"更容易诊断 —— 失败发生在取变量处，而不是几步之后的 VFS 解析里。
// =======================================================================================
const char* guestUserProfileText();    // C:\Users\Amayui
const char* guestAppDataText();        // C:\Users\Amayui\AppData\Roaming
const char* guestLocalAppDataText();   // C:\Users\Amayui\AppData\Local
const char* guestDocumentsText();      // C:\Users\Amayui\Documents
// TEMP / TMP / GetTempPathA 的共同落点（`…\AppData\Local\Temp`；需要 --appdata 才可解析）
std::string guestTempDirText();

// guest 环境块：有序 `NAME=VALUE` 列表（顺序 = 大小写不敏感的字典序，**确定性**）。
// `have_appdata=false` ⇒ 不含 APPDATA / LOCALAPPDATA / TEMP / TMP（见上面的理由）。
std::vector<std::string> guestEnvironment(bool have_appdata);
// 同一份内容的 `;` 连接文本：`ProcessSetup::environment_ansi` 与 A4 的 trace 输入哈希用它。
std::string guestEnvironmentText(bool have_appdata);

class Vfs {
public:
    // 宿主根（绝对路径，UTF-8）。空 ⇒ ready() = false。
    void setGameDir(const std::string& host_dir);
    const std::string& gameDir() const { return game_dir_; }
    // ★ P0：伪造 AppData（--appdata）。只影响特殊目录（AppData / LocalAppData / Personal），
    //   不改变 --game-dir 是唯一游戏根的口径。空 = 未设置（特殊目录解析会响亮失败）。
    //   ★ 设置时会清掉 `<--appdata>/Local/Temp`（A4：`GetTempFileNameA(uUnique=0)` 的序号
    //     跨运行累积会让同一条命令第二次拿到不同名字 ⇒ trace 不再逐字节相同）。
    void setAppDataDir(const std::string& host_dir);
    const std::string& appDataDir() const { return app_data_dir_; }
    bool ready() const { return !game_dir_.empty(); }
    // guest 环境块的真源（= `guestEnvironment(!appDataDir().empty())`）
    std::vector<std::string> guestEnvironment() const { return lift::guestEnvironment(!app_data_dir_.empty()); }
    std::string guestEnvironmentText() const { return lift::guestEnvironmentText(!app_data_dir_.empty()); }

    // guest 根的文本形式（缺省 `E:\Games\Eushully\天結`；只用于把路径的公共前缀剥掉与日志）
    void setGuestRoot(const std::string& guest_root) { guest_root_ = guest_root; }
    const std::string& guestRoot() const { return guest_root_; }

    // ★ guest 眼里的主模块路径 = **guest 根之下的 `AGE.EXE`**（用户 2026-11-05 口径：
    //   引擎从 exe 路径推导"安装目录"时，必须得到游戏目录本身，而不是它的父目录）。
    //   ⛔ 宿主 `--exe` 字符串不再出现在 `GetModuleFileNameA` / `GetCommandLineA` 里。
    std::string guestExePath() const;

    // ★ VFS overlay（只对**这一个文件**生效；⛔ 不是通用路径重定向）：
    //   guest 打开 `guest_path` 时拿到的是 `host_path` 的字节（本模拟器真正在跑的拆壳件），
    //   而不是 `--game-dir` 下那份加壳原件。匹配 = 绝对化后的 guest 路径按段大小写不敏感相等，
    //   所以 `E:\…\AGE.EXE` / `e:\…\age.exe` / 相对路径（cwd = guest 根）都会命中。
    //   命中时 `resolve` 打一行日志（来源 + sha256 前 8 位）。
    void setExeOverlay(const std::string& guest_path, const std::string& host_path, const std::string& sha256);
    bool hasExeOverlay() const { return !overlay_guest_.empty(); }
    const std::string& overlayGuestPath() const { return overlay_guest_; }
    const std::string& overlayHostPath() const { return overlay_host_; }

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
    // ★ overlay 命中：`guest_path` 是否就是被覆盖的那一个文件（绝对化 + 大小写不敏感按段比较）
    bool matchesOverlay(const std::string& guest_path) const;

    std::string game_dir_;
    std::string app_data_dir_;
    std::string guest_root_ = "E:\\Games\\Eushully\\天結";
    std::string guest_cwd_;
    bool guest_cwd_set_ = false;
    // overlay（只允许一个文件；见 setExeOverlay）
    std::string overlay_guest_;
    std::string overlay_host_;
    std::string overlay_sha256_;
};

// 跨平台 FILE_ATTRIBUTE_*（只实现本线用到的几个）
constexpr std::uint32_t kAttrReadOnly = 0x00000001u;
constexpr std::uint32_t kAttrDirectory = 0x00000010u;
constexpr std::uint32_t kAttrArchive = 0x00000020u;
constexpr std::uint32_t kAttrNormal = 0x00000080u;

}  // namespace lift
