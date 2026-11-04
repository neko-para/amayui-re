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
#include <string>
#include <vector>

namespace lift {

struct HostDirEntry {
    std::string name;      // 宿主目录里的**真实**文件名（UTF-8）
    bool is_dir = false;
    bool is_file = false;
    std::uint64_t size = 0;
    std::uint32_t attributes = 0;  // FILE_ATTRIBUTE_* 的跨平台近似
    std::int64_t write_time_100ns = 0;  // 自 1601-01-01 起的 100ns（Windows FILETIME）
};

class Vfs {
public:
    // 宿主根（绝对路径，UTF-8）。空 ⇒ ready() = false。
    void setGameDir(const std::string& host_dir);
    const std::string& gameDir() const { return game_dir_; }
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
