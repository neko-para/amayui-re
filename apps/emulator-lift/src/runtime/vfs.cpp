// emulator-lift · VFS 实现：Win32 guest 路径 ↔ 宿主路径（唯一根 = `--game-dir`）
//
// ★ 合同在 `vfs.h`（本文件只是它的实现）。口径与风险点（plan §6 S6 / §9 R2）：
//   · **唯一根**：guest 绝对路径按 `resolve` 的顺序匹配（见 vfs.h：overlay → guest 根前缀 →
//     `--appdata` 前缀 → 相对路径）；`guest 根前缀` 剥掉之后的余下部分**照旧**走分段解析；
//     `..` 越出根 ⇒ **失败**（⛔ 不夹回根里）。
//   · **guest 路径文本的编码**（2026-11-05 缺陷单 `🐞S2`）：入口用 `guestPathForLog` 归一成
//     UTF-8（guest 内存里的 A 版字节是 CP932）；规则只有一条，见 vfs.h。
//   · **错误串带 tag 前缀**（`[notfound]` / `[notdir]` / `[escape]` / `[ambiguous]` / `[invalid]`
//     / `[io]` / `[ready]`）：shim 侧靠 tag 映射成 Win32 错误码（映射表在 kernel32_file.cpp）。
//     ⛔ 别改 tag 文本而不改那张映射表。
//   · **大小写**：先精确匹配；否则在宿主目录项里做**只对 ASCII 段**的大小写不敏感**唯一**匹配
//     （日文名没有大小写 ⇒ 非 ASCII 段只做精确匹配）。多个候选 ⇒ `[ambiguous]`：Windows 上不会
//     发生；Linux/macOS 上必须显式失败，⛔ 不能按目录顺序猜一个。
//   · **枚举顺序**：`find()` 永远 `std::sort` 后返回（UTF-8 字节序，确定性优先于"像 Windows"）。
//     ⛔ 绝不用 `directory_iterator` 的默认顺序当判据（plan §8.3-4）。
//   · **通配符**只实现游戏实际用到的形态：`*`（任意串）/ `?`（**一个 UTF-8 码点**）/ `*.*`（匹配全部），
//     另按 Windows 语义补一条"尾部 `.*` 可省"（`foo.*` 也匹配 `foo`）。非法形态（`**` / 模式段为空
//     或以分隔符结尾 / 模式段是 `.` 或 `..`）⇒ 失败 + error，⛔ 不静默忽略。
//   · **时间**：三个时间（创建 / 访问 / 修改）走 **SDL3 `SDL_GetPathInfo`**（平台真值，实现里没有
//     `#ifdef`）；只有 SDL 失败时才退回 `std::filesystem::last_write_time`，并用
//     `fileTime100ns` 的"此刻两钟差值"近似 ⇒ 三值相同、只在同一次运行内单调。
//   · **宿主路径一律 UTF-8 字符串**。C++20 下用 `char8_t` 构造/取出 `fs::path`（与已弃用的
//     `u8path` 语义相同，但不触发弃用警告）；Windows 上 `fs::path` 内部是 wchar_t。
//   · 这是**叶子设施**：不缓存、不碰 guest 内存、不打日志（shim 负责这些）。
//
// ⚠ 已知边界（M1 口径，按 plan §9 R2 "先只实现游戏实际用到的形态"）：
//   1. **8.3 短名不模拟**：`FindFirstFileA` 的 `cAlternateFileName` 由 shim 填 0；拿短名当输入
//      解析会 `[notfound]`。
//   2. **尾随点 / 尾随空格不折叠**：Windows 会去掉文件名尾部的 `.` / ` `，这里按字节原样处理
//      （Linux/macOS 上也必须原样，否则跨平台不一致）。
//   3. **一个盘符对多个根**：所有盘符都剥掉并映射到 game_dir（本模拟器只有一张"盘"）。
//   4. **`..` 的折叠只看文本**：不做符号链接感知（不 realpath）。宿主目录里若放了目录符号链接，
//      穿过它之后的路径仍按文本判定"在根下"；⛔ 这不是安全边界，只是路径映射。
#include "runtime/vfs.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_time.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "runtime/cp932.h"

#if defined(__APPLE__)
#include <sys/stat.h>  // st_birthtimespec（SDL_GetPathInfo 在 POSIX 给的是 ctime）
#endif

namespace lift {
namespace {

namespace fs = std::filesystem;

// 1601-01-01 → 1970-01-01 的 100 ns 数（与 kernel32_time.cpp 的 kFiletimeEpochDelta 同一个常数）
constexpr std::int64_t kFileTimeEpochDelta = 116444736000000000ll;

// ---------------------------------------------------------------------------------------
// 编码：UTF-8 字符串 ↔ fs::path
// ---------------------------------------------------------------------------------------
fs::path pathFromUtf8(std::string_view s) {
    const std::u8string u(reinterpret_cast<const char8_t*>(s.data()), s.size());
    return fs::path(u);
}

std::string pathToUtf8(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

// ---------------------------------------------------------------------------------------
// ASCII 小工具（大小写折叠**只对 ASCII**：日文名没有大小写）
// ---------------------------------------------------------------------------------------
char asciiLowerChar(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c; }

bool asciiEqualNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (asciiLowerChar(a[i]) != asciiLowerChar(b[i])) return false;
    }
    return true;
}

bool isAscii(std::string_view s) {
    for (const char c : s) {
        if (static_cast<unsigned char>(c) >= 0x80) return false;
    }
    return true;
}

// UTF-8 合法性（整串；strict：过短编码 / 代理项 / > U+10FFFF 都不算）。
// ★ 只用来**选解码器**（见 guestPathForLog）——它不是安全校验，也不改变任何解析语义。
bool isValidUtf8(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80u) {
            ++i;
            continue;
        }
        std::size_t n = 0;  // 续字节个数
        std::uint32_t cp = 0;
        if ((c & 0xE0u) == 0xC0u) {
            n = 1;
            cp = c & 0x1Fu;
        } else if ((c & 0xF0u) == 0xE0u) {
            n = 2;
            cp = c & 0x0Fu;
        } else if ((c & 0xF8u) == 0xF0u) {
            n = 3;
            cp = c & 0x07u;
        } else {
            return false;  // 0x80..0xBF 的孤立续字节 / 0xF8.. 的非法首字节
        }
        if (i + n >= s.size()) return false;  // 截断
        for (std::size_t k = 1; k <= n; ++k) {
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0u) != 0x80u) return false;
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if ((n == 1 && cp < 0x80u) || (n == 2 && cp < 0x800u) || (n == 3 && cp < 0x10000u)) return false;
        if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) return false;
        i += n + 1;
    }
    return true;
}

std::string normalizeSeps(std::string s) {
    for (char& c : s) {
        if (c == '\\') c = '/';
    }
    return s;
}

void trimLeadingSeps(std::string& s) {
    std::size_t n = 0;
    while (n < s.size() && s[n] == '/') ++n;
    if (n) s.erase(0, n);
}

// 去尾部隔离符（两种都去：guest 文本形式里 `\` 与 `/` 都可能出现）
std::string trimTrailingSeps(std::string s) {
    while (s.size() > 1 && (s.back() == '/' || s.back() == '\\')) s.pop_back();
    return s;
}

bool hasDrive(std::string_view s) {
    return s.size() >= 2 &&
           ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z')) && s[1] == ':';
}

std::string stripDrive(std::string s) {
    if (hasDrive(s)) s.erase(0, 2);
    return s;
}

// guest 当前目录：`guestCwd()` 空 ⇒ `guestRoot()`（vfs.h 的合同："缺省 = guestRoot"）。
// ★ 为什么不在 ctor 里把 guest_cwd_ 填成 guest_root_：`setGuestRoot` 是 inline 的，调用顺序
//   （先 setGameDir 还是先 setGuestRoot）由装配点决定；延迟取值才不会被那个顺序咬到。
std::string cwdOf(const Vfs& v) {
    if (!v.guestCwd().empty()) return v.guestCwd();
    if (!v.guestRoot().empty()) return v.guestRoot();
    return std::string("C:\\");
}

// `text` 是否以目录形式 `form` 开头（form 之后必须是 '/' 或结尾）
bool prefixIsDir(std::string_view text, std::string_view form) {
    if (form.empty() || text.size() < form.size()) return false;
    if (!asciiEqualNoCase(text.substr(0, form.size()), form)) return false;
    return text.size() == form.size() || text[form.size()] == '/';
}

std::string joinGuest(const std::string& dir, const std::string& rel) {
    if (rel.empty()) return dir;
    std::string base = trimTrailingSeps(dir);
    if (base.empty()) return rel;
    return base + "/" + rel;
}

// =======================================================================================
// ★ guest 根前缀这一档（**唯一**的"剥壳"实现：`isAbsoluteText` / `splitGuestImpl` /
//   `Vfs::resolve` 共用）
//
//   `E:\Games\Eushully\天結\AGE.EXE` ⇒ 余下 `AGE.EXE`（相对 `--game-dir` 根）。
//   · 变体全认：`\` 与 `/`、ASCII 大小写、末尾可有分隔符、盘符可有可无（本模拟器只有一张"盘"，
//     见文件头的边界 3）；
//   · **只认完整段前缀**：`…\Eushully\…` / `…\天結X` / 根的文本只出现在中间（`C:\backup\…`）
//     一律**不**命中 —— ⛔ 不是"路径里含根的某一段就剥"（那会把备份路径 / 中间段当成根）；
//   · ★ 剥出来的余下部分**不是**宿主路径：它还要走 `splitRelativeText`（`..` 归一化 + `[escape]`）
//     与 resolve 的分段解析（大小写不敏感 / overlay / 越界检查）。
// =======================================================================================
bool stripGuestRootPrefix(const Vfs& v, const std::string& text_in, std::string* rest) {
    rest->clear();
    std::string text = stripDrive(normalizeSeps(text_in));
    trimLeadingSeps(text);
    std::string root = stripDrive(trimTrailingSeps(normalizeSeps(v.guestRoot())));
    trimLeadingSeps(root);
    if (root.empty() || !prefixIsDir(text, root)) return false;
    *rest = text.substr(root.size());
    trimLeadingSeps(*rest);
    return true;
}

// guest 文本是不是"绝对路径"（盘符 / 前导分隔符 / guest 根前缀）。
// ★ 复用 stripGuestRootPrefix：根前缀只有这一份实现（两份必然漂）。
bool isAbsoluteText(const Vfs& v, const std::string& text) {
    if (hasDrive(text)) return true;
    if (!text.empty() && text[0] == '/') return true;
    std::string rest;
    return stripGuestRootPrefix(v, text, &rest);
}

// ---------------------------------------------------------------------------------------
// guest 路径 → 段序列（已折叠 `.` / `..`）
// ---------------------------------------------------------------------------------------
bool applySegment(const std::string& seg, const std::string& original, std::vector<std::string>* segs,
                  std::string* error, bool clamp_escape = false) {
    if (seg.empty() || seg == ".") return true;
    if (seg == "..") {
        if (segs->empty()) {
            // clamp_escape：`..` 已经在根上 ⇒ 停在根（GetFullPathName 的文本语义），不是错误
            if (clamp_escape) return true;
            // ★ 日志文本走 guestPathForLog：guest 内存里的 A 版字节是 CP932，
            //   直接写进 UTF-8 的 stderr 会变成 `?V??`（2026-11-05 缺陷单）。
            *error = "[escape] 路径 `" + guestPathForLog(original) + "` 里的 `..` 越出 guest 根（唯一根是 game_dir）";
            return false;
        }
        segs->pop_back();
        return true;
    }
    segs->push_back(seg);
    return true;
}

// 把一段**相对 / 已剥掉根前缀**的文本切成段（折叠 `.` / `..`；`..` 越界 ⇒ `[escape]`）。
// ★ guest 根前缀那一档剥完之后走的就是这里 —— 与相对路径**同一条**代码路径，
//   ⛔ 不许"剥完前缀直接拼宿主路径"。
bool splitRelativeText(const std::string& text, const std::string& original,
                       std::vector<std::string>* segs, std::string* error, bool clamp_escape) {
    segs->clear();
    std::string cur;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '/') {
            if (!applySegment(cur, original, segs, error, clamp_escape)) return false;
            cur.clear();
        } else {
            cur.push_back(text[i]);
        }
    }
    return true;
}

bool splitGuestImpl(const Vfs& v, const std::string& guest_path, std::vector<std::string>* segs,
                    std::string* error, bool clamp_escape) {
    segs->clear();
    // ★ 入口归一（规则见 vfs.h）：guest 内存里的 A 版路径是 CP932 ⇒ 先变成 UTF-8 文本。
    //   之后的所有比较（guest 根前缀 / 大小写 / overlay）都在同一套字节上做。
    const std::string guest = guestPathForLog(guest_path);
    std::string text = normalizeSeps(guest);
    if (text.empty()) text = cwdOf(v);
    if (!isAbsoluteText(v, text)) text = joinGuest(cwdOf(v), text);
    // ★ 拼过 cwd 之后必须再规范化一次：`guestRoot()` / `guestCwd()` 的**文本形式用的是 `\`**
    //   （vfs.h 的缺省根是 `E:\Games\Eushully\天結`）⇒ 不重规范化会得到混合隔离符，
    //   与根前缀的形式比不中，整条相对路径会解析到 game_dir/Games/... 下面。
    text = normalizeSeps(text);
    // ★ ② guest 根前缀（最具体）：`E:\Games\Eushully\天結\A\B` ⇒ `A/B`
    std::string rest;
    if (stripGuestRootPrefix(v, text, &rest)) {
        return splitRelativeText(rest, guest, segs, error, clamp_escape);
    }
    // ④ 相对路径：剥盘符与前导分隔符（本模拟器只有一张"盘"）
    text = stripDrive(text);
    trimLeadingSeps(text);
    return splitRelativeText(text, guest, segs, error, clamp_escape);
}

bool splitGuest(const Vfs& v, const std::string& guest_path, std::vector<std::string>* segs,
                std::string* error) {
    return splitGuestImpl(v, guest_path, segs, error, /*clamp_escape=*/false);
}

// ---------------------------------------------------------------------------------------
// 目录项查询（大小写不敏感的唯一匹配）
// ---------------------------------------------------------------------------------------
enum class SegLookup { Found, Missing, Error };

SegLookup lookupSegment(const fs::path& dir, const std::string& seg, bool exact_only,
                        std::string* real_name, std::string* error) {
    real_name->clear();
    if (seg.empty()) {
        *error = "[invalid] 空路径段";
        return SegLookup::Error;
    }
    std::error_code ec;
    const fs::path exact = dir / pathFromUtf8(seg);
    if (fs::exists(exact, ec) && !ec) {
        *real_name = seg;
        return SegLookup::Found;
    }
    if (exact_only) return SegLookup::Missing;
    // 非 ASCII 段：只做精确匹配（日文名没有大小写）。
    if (!isAscii(seg)) return SegLookup::Missing;

    ec.clear();  // ← 上面 exists() 可能留下过错误码，别让它干扰下面的枚举
    if (!fs::is_directory(dir, ec) || ec) {
        *error = "[notdir] \"" + pathToUtf8(dir) + "\" 不是可枚举的宿主目录";
        return SegLookup::Error;
    }
    std::vector<std::string> hits;
    fs::directory_iterator it(dir, ec);
    if (ec) {
        *error = "[io] 枚举 \"" + pathToUtf8(dir) + "\" 失败：" + ec.message();
        return SegLookup::Error;
    }
    const fs::directory_iterator end;
    while (it != end) {
        const std::string name = pathToUtf8(it->path().filename());
        if (asciiEqualNoCase(name, seg)) hits.push_back(name);
        it.increment(ec);
        if (ec) {
            *error = "[io] 枚举 \"" + pathToUtf8(dir) + "\" 中断：" + ec.message();
            return SegLookup::Error;
        }
    }
    if (hits.size() > 1) {
        std::sort(hits.begin(), hits.end());
        std::string list;
        for (const std::string& h : hits) {
            if (!list.empty()) list += ", ";
            list += h;
        }
        *error = "[ambiguous] 段 \"" + seg + "\" 在 \"" + pathToUtf8(dir) + "\" 里有多个大小写不敏感候选：[" +
                 list + "] —— 不按目录顺序猜";
        return SegLookup::Error;
    }
    if (hits.size() == 1) {
        *real_name = hits[0];
        return SegLookup::Found;
    }
    return SegLookup::Missing;
}

// 结果必须落在根下：逐段比较（路径是我们自己从 game_dir 拼的 ⇒ 精确比较足够）
bool isUnderRoot(const fs::path& root, const fs::path& p) {
    auto r = root.begin();
    auto q = p.begin();
    for (; r != root.end(); ++r, ++q) {
        if (q == p.end()) return false;
        if (*r != *q) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------
// 属性 / 时间
// ---------------------------------------------------------------------------------------
std::uint32_t attributesOfStatus(const fs::file_status& st) {
    if (fs::is_directory(st)) return kAttrDirectory;
    std::uint32_t a = kAttrArchive;
    if ((st.permissions() & fs::perms::owner_write) == fs::perms::none) a |= kAttrReadOnly;
    return a;
}

// `file_time_type` → FILETIME 100 ns（自 1601）。
// ★ 近似方式：取**此刻**两个钟的时间点做差（`file_time_type::clock::now()` 与 `system_clock::now()`），
//   把待转换的时间点平移到 system_clock 上。同一进程内平移量是常数 ⇒ 同一批文件之间的**相对**
//   顺序/间隔是准的；**绝对值**只保证"和宿主 mtime 一致"，不保证跨机器 / 跨时刻可复现
//   （plan §8.3 的确定性要求只覆盖 guest 可见的时间源 = 虚拟时钟；文件时间戳属宿主事实）。
std::int64_t fileTime100ns(fs::file_time_type t) {
    using hundred_ns = std::chrono::duration<std::int64_t, std::ratio<1, 10000000>>;
    const auto now_file = fs::file_time_type::clock::now();
    const auto now_sys = std::chrono::system_clock::now();
    const auto delta = t - now_file;
    const auto sys = now_sys + std::chrono::duration_cast<std::chrono::system_clock::duration>(delta);
    return std::chrono::duration_cast<hundred_ns>(sys.time_since_epoch()).count() + kFileTimeEpochDelta;
}

// ---------------------------------------------------------------------------------------
// 通配符（`*` / `?` / `*.*`）
// ---------------------------------------------------------------------------------------
std::size_t utf8CharLen(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;  // 非法首字节：按单字节走（不会更坏）
}

bool matchImpl(std::string_view p, std::string_view s) {
    std::size_t pi = 0;
    std::size_t si = 0;
    std::size_t star = std::string_view::npos;
    std::size_t mark = 0;
    while (si < s.size()) {
        if (pi < p.size() && p[pi] == '?') {
            const std::size_t n = utf8CharLen(static_cast<unsigned char>(s[si]));
            if (si + n > s.size()) return false;
            ++pi;
            si += n;
        } else if (pi < p.size() && p[pi] == '*') {
            star = pi++;
            mark = si;
        } else if (pi < p.size() && asciiLowerChar(p[pi]) == asciiLowerChar(s[si])) {
            ++pi;
            ++si;
        } else if (star != std::string_view::npos) {
            pi = star + 1;
            si = ++mark;
        } else {
            return false;
        }
    }
    while (pi < p.size() && p[pi] == '*') ++pi;
    return pi == p.size();
}

bool wildcardMatch(const std::string& pat, const std::string& name) {
    if (pat == "*.*") return true;  // Windows 语义：`*.*` 匹配全部（含没有扩展名的）
    if (matchImpl(pat, name)) return true;
    // Windows 语义：尾部 `.*` 可省（`foo.*` 也匹配 `foo`）
    if (pat.size() >= 2 && pat.compare(pat.size() - 2, 2, ".*") == 0) {
        return matchImpl(std::string_view(pat).substr(0, pat.size() - 2), name);
    }
    return false;
}

}  // namespace

// =======================================================================================
// guest 路径文本的编码归一（规则见 vfs.h；**解析入口与日志共用这一条规则**）
// =======================================================================================
std::string guestPathForLog(std::string_view guest_path) {
    if (guest_path.empty()) return std::string();
    // 已经是 UTF-8（Vfs 内部口径 / 宿主侧文本，含纯 ASCII）⇒ 原样。
    if (isValidUtf8(guest_path)) return std::string(guest_path);
    // 否则按 guest 内存里的 A 版字节（CP932）解；非法字节按 Windows 的宽松口径跳过
    // （"跳过并计数"在解码器里做；这里只要一行可读的日志文本 ⇒ 不另记）。
    return utf16ToUtf8(cp932ToWide(guest_path));
}

// =======================================================================================
// 宿主文件时间
//   · Windows：SDL_GetPathInfo 的 create_time 就是 ftCreationTime；
//   · macOS：SDL 的 POSIX 实现把 **ctime** 当 create_time（实测 3.4.18：create == ctime == mtime），
//     所以这里用 `stat` 的 `st_birthtimespec`（用户 2026-11-04 口径：有 birthtime 就用，否则回退 ctime）；
//   · 其它 POSIX：没有可移植的 birthtime ⇒ 接受 SDL 的 ctime 回退。
// =======================================================================================
bool hostFileTimes(const std::string& host_path, HostFileTimes* out) {
    if (!out) return false;
    // SDL_Time = 自 1970 起的**纳秒**；`SDL_TimeToWindows` 转成 FILETIME（100ns 自 1601）。
    // ★ 0 表示"这个文件系统不提供该时间" ⇒ 按 Windows 文档留 FILETIME 0，⛔ 不编一个 1970。
    auto toWindows100ns = [](SDL_Time t) -> std::uint64_t {
        if (t <= 0) return 0;
        Uint32 lo = 0;
        Uint32 hi = 0;
        SDL_TimeToWindows(t, &lo, &hi);
        return (static_cast<std::uint64_t>(hi) << 32) | lo;
    };
#if defined(__APPLE__)
    struct stat st{};
    if (::stat(host_path.c_str(), &st) != 0) return false;
    auto nsOf = [](const struct timespec& t) -> SDL_Time {
        return static_cast<SDL_Time>(t.tv_sec) * 1000000000ll + static_cast<SDL_Time>(t.tv_nsec);
    };
    out->create_100ns = toWindows100ns(nsOf(st.st_birthtimespec));
    out->access_100ns = toWindows100ns(nsOf(st.st_atimespec));
    out->write_100ns = toWindows100ns(nsOf(st.st_mtimespec));
    return true;
#else
    SDL_PathInfo info{};
    if (!SDL_GetPathInfo(host_path.c_str(), &info)) return false;
    out->create_100ns = toWindows100ns(info.create_time);
    out->access_100ns = toWindows100ns(info.access_time);
    out->write_100ns = toWindows100ns(info.modify_time);
    return true;
#endif
}

// 宿主 UTF-8 路径 → FILE*。Windows 必须走宽字符版（见 vfs.h 的说明）。
std::FILE* openHostFileUtf8(const std::string& host_path, const char* mode) {
#if defined(_WIN32)
    const std::wstring wp = pathFromUtf8(host_path).wstring();
    const std::wstring wm(mode, mode + std::strlen(mode));
    std::FILE* fp = nullptr;
    if (::_wfopen_s(&fp, wp.c_str(), wm.c_str()) != 0) return nullptr;
    return fp;
#else
    return std::fopen(host_path.c_str(), mode);
#endif
}

// =======================================================================================
// guest 特殊目录 / 环境块（唯一文本真源；见 vfs.h 的说明）
// =======================================================================================
const char* guestUserProfileText() { return "C:\\Users\\Amayui"; }
const char* guestAppDataText() { return "C:\\Users\\Amayui\\AppData\\Roaming"; }
const char* guestLocalAppDataText() { return "C:\\Users\\Amayui\\AppData\\Local"; }
const char* guestDocumentsText() { return "C:\\Users\\Amayui\\Documents"; }

std::string guestTempDirText() {
    std::string s = guestLocalAppDataText();
    s += "\\Temp";
    return s;
}

// 有序 `NAME=VALUE`（大小写不敏感的字典序 = 确定性）。⛔ 不读宿主环境：同一台机器上重跑
// 必须逐字节相同（A4），宿主的 LOCALAPPDATA / USERNAME 一律不进 guest。
std::vector<std::string> guestEnvironment(bool have_appdata) {
    std::vector<std::string> out;
    auto add = [&out](const char* name, const std::string& value) {
        out.push_back(std::string(name) + "=" + value);
    };
    // AppData 派生的四项：只有 --appdata 存在时才给（两个视图说同一件事，见 vfs.h）。
    if (have_appdata) {
        add("APPDATA", guestAppDataText());
    }
    add("COMSPEC", "C:\\Windows\\System32\\cmd.exe");
    add("HOMEDRIVE", "C:");
    add("HOMEPATH", "\\Users\\Amayui");
    if (have_appdata) {
        add("LOCALAPPDATA", guestLocalAppDataText());
    }
    // PATH 只放 `.`：本模拟器没有 system32 / Windows 目录（所有盘符都映射到 --game-dir），
    // 放一串宿主路径只会造出"能查但打不开"的假象。CRT 只用它找 DLL，`.` 是安全的最小值。
    add("PATH", ".");
    add("SystemDrive", "C:");
    // SystemRoot / windir 是 Windows 的**同义**两项（历史原因两边都有），值必须一致；
    // 它们指向 `C:\Windows` ⇒ 经 VFS 落到 game_dir\Windows（本模拟器没有那棵树 ⇒ 打不开是响亮的）。
    add("SystemRoot", "C:\\Windows");
    if (have_appdata) {
        add("TEMP", guestTempDirText());
        add("TMP", guestTempDirText());
    }
    add("USERPROFILE", guestUserProfileText());
    add("windir", "C:\\Windows");
    return out;
}

std::string guestEnvironmentText(bool have_appdata) {
    const std::vector<std::string> items = guestEnvironment(have_appdata);
    std::string out;
    for (const std::string& s : items) {
        if (!out.empty()) out += ';';
        out += s;
    }
    return out;
}

// =======================================================================================
// Vfs
// =======================================================================================
void Vfs::setGameDir(const std::string& host_dir) {
    if (host_dir.empty()) {
        game_dir_.clear();
        guest_cwd_.clear();
        guest_cwd_set_ = false;
        return;
    }
    std::error_code ec;
    fs::path p = fs::absolute(pathFromUtf8(host_dir), ec);
    if (ec) p = pathFromUtf8(host_dir);
    p = p.lexically_normal();
    // 去掉尾部隔离符（保留 "C:\" 这种根）
    if (p != p.root_path() && !p.has_filename()) p = p.parent_path();
    game_dir_ = pathToUtf8(p);
    // ★ 2026-11-05：**不再**在这里清 `game_dir/.lift-tmp` —— 那是对 `--game-dir`（安装树）的写。
    //   临时目录的落点已改成 `<--appdata>/Local/Temp`（见 setAppDataDir / guestTempDirText）。
}

void Vfs::setAppDataDir(const std::string& host_dir) {
    app_data_dir_ = host_dir;
    if (app_data_dir_.empty()) return;
    // ★ A4（逐字节可复现）：清掉上一轮的 Temp。`GetTempFileNameA(uUnique=0)` 从 0 起找空号 ⇒
    //   目录跨运行累积会让同一条命令第二次拿到不同序号（返回路径 / trace 都会变）。
    //   这是本模拟器**自有**的伪造 AppData，删它不影响任何真实数据；失败只影响确定性。
    //   ⛔ 不要把它指向真实用户目录：这里会连同 `<appdata>/Local/Temp` 一起清掉（`--appdata`
    //   的语义就是"可丢弃的伪造根"）。
    std::error_code ec;
    fs::remove_all(pathFromUtf8(app_data_dir_) / "Local" / "Temp", ec);
}

std::string Vfs::guestExePath() const {
    std::string root = guest_root_;
    while (root.size() > 1 && (root.back() == '\\' || root.back() == '/')) root.pop_back();
    if (root.empty()) root = "C:";
    if (root.size() == 2 && root[1] == ':') root += '\\';  // "C:" → "C:\"
    if (root.back() != '\\') root += '\\';
    return root + "AGE.EXE";
}

void Vfs::setExeOverlay(const std::string& guest_path, const std::string& host_path,
                        const std::string& sha256) {
    overlay_guest_.clear();
    overlay_host_.clear();
    overlay_sha256_.clear();
    if (guest_path.empty() || host_path.empty()) return;
    overlay_guest_ = absoluteGuest(guest_path);
    for (char& c : overlay_guest_) {
        if (c == '/') c = '\\';  // guest 世界用反斜杠（日志 / toGuest 的反解都按这个形态）
    }
    overlay_host_ = host_path;
    overlay_sha256_ = sha256;
}

bool Vfs::matchesOverlay(const std::string& guest_path) const {
    if (overlay_guest_.empty()) return false;
    const std::string canon = normalizeSeps(absoluteGuest(guest_path));
    return !canon.empty() && asciiEqualNoCase(canon, normalizeSeps(overlay_guest_));
}


void Vfs::setGuestCwd(const std::string& guest_path) {
    guest_cwd_ = absoluteGuest(guest_path);
    guest_cwd_set_ = true;
    // `guest_cwd_set_` 只表达"调用方显式设过"（缺省值口径见 cwdOf）；读一次免得它变成
    // "只写不读的私有字段"（-Wunused-private-field 会响）。
    (void)guest_cwd_set_;
}

namespace {
// P0 的伪造 AppData 前缀：guest 看到的仍是 Windows 形状，宿主落到 --appdata。
// ★ 文本来自 vfs.h 的**唯一真源**（与 env 块 / SHGetSpecialFolderPathA 同一份；⛔ 不再各写一份）。
struct SpecialRoot {
    std::string guest;  // 归一化成 `/` 的形式（前缀比较用）
    const char* sub;    // --appdata 下的子目录
};
const std::vector<SpecialRoot>& specialRoots() {
    static const std::vector<SpecialRoot> kRoots = {
        {normalizeSeps(guestAppDataText()), "Roaming"},
        {normalizeSeps(guestLocalAppDataText()), "Local"},
        {normalizeSeps(guestDocumentsText()), "Documents"},
    };
    return kRoots;
}
}  // namespace

bool Vfs::resolve(const std::string& guest_path, std::string* host_path, std::string* error,
                  bool must_exist, bool resolve_case) const {
    if (host_path) host_path->clear();
    if (error) error->clear();
    if (!ready()) {
        if (error) *error = "[ready] game_dir 未设置（--game-dir 是唯一根）";
        return false;
    }
    if (guest_path.empty()) {
        if (error) *error = "[invalid] 空路径";
        return false;
    }
    // ★ 入口归一（规则见 vfs.h）：guest 内存里的 A 版路径是 CP932 字节 ⇒ 变成 UTF-8 文本。
    //   从这里往下只有**一套字节**（UTF-8）在比较 / 打印 —— 这既让 `E:\Games\Eushully\天結\…`
    //   的根前缀能剥（2026-11-05 缺陷单：CP932 与 UTF-8 的 `天結` 字节不同 ⇒ 剥不掉 ⇒
    //   `[notdir] 段 "Games"`），也让日志不再把 CP932 字节写进 UTF-8 流。
    const std::string guest = guestPathForLog(guest_path);
    // ★ ① overlay（只对**一个**文件）：命中 ⇒ 宿主路径直接是真正在跑的那份字节。
    //   命中时打一行日志：来源 + sha256 前 8 位（用户 2026-11-05 口径；这是**唯一**一条
    //   "Vfs 自己打日志"的路径 —— 它足够罕见，而且必须能从日志里看见走的是哪一份）。
    if (matchesOverlay(guest)) {
        std::fprintf(stderr, "[vfs] overlay: guest \"%s\" ⇒ \"%s\"（来源：--exe 拆壳件；sha256 前 8 位 %s）\n",
                     guest.c_str(), overlay_host_.c_str(),
                     overlay_sha256_.empty() ? "-" : overlay_sha256_.substr(0, 8).c_str());
        if (host_path) *host_path = overlay_host_;
        return true;
    }

    std::string err;
    std::vector<std::string> segs;

    // ★ ② guest 根前缀（**最具体**，先于 appdata 与相对路径）：`<guest_root>\…` ⇒ `--game-dir` 根。
    //   ★ 剥完之后**必须**再走一遍分段解析（大小写不敏感 / `..` 归一化 / overlay / `[escape]`）——
    //     这里只把"根之后的余下文本"交给 splitRelativeText，⛔ 不直接拼宿主路径。
    std::string root_rel;
    const bool rooted = stripGuestRootPrefix(*this, guest, &root_rel);
    if (rooted) {
        if (!splitRelativeText(root_rel, guest, &segs, &err, /*clamp_escape=*/false)) {
            if (error) *error = err;
            return false;
        }
    } else {
        // ★ ③ 特殊目录（--appdata）：匹配固定的 guest 前缀，剩余段落到 appdata/<sub>。
        const std::string special_text = normalizeSeps(guest);
        for (const SpecialRoot& sr : specialRoots()) {
            const std::string prefix = sr.guest;
            if (!prefixIsDir(special_text, prefix)) continue;
            if (app_data_dir_.empty()) {
                if (error) *error = std::string("[appdata] 特殊目录 \"") + sr.guest +
                                    "\" 需要 --appdata，但当前为空";
                return false;
            }
            const std::string rest = special_text.substr(prefix.size());
            std::vector<std::string> ssegs;
            std::string seg_err;
            if (!splitRelativeText(rest, guest, &ssegs, &seg_err, /*clamp_escape=*/false)) {
                if (error) *error = seg_err;
                return false;
            }
            const fs::path sroot = (pathFromUtf8(app_data_dir_) / pathFromUtf8(sr.sub)).lexically_normal();
            fs::path scur = sroot;
            std::string serr;
            for (std::size_t i = 0; i < ssegs.size(); ++i) {
                const bool last = (i + 1 == ssegs.size());
                std::string real;
                const SegLookup r = lookupSegment(scur, ssegs[i], last && !resolve_case, &real, &serr);
                if (r == SegLookup::Error) {
                    if (error) *error = serr;
                    return false;
                }
                if (r == SegLookup::Found) {
                    scur /= pathFromUtf8(real);
                    continue;
                }
                if (!last || must_exist) {
                    if (error) *error = std::string(last ? "[notfound]" : "[notdir]") + " 段 \"" + ssegs[i] +
                                        "\" 在 \"" + pathToUtf8(scur) + "\" 里不存在";
                    return false;
                }
                scur /= pathFromUtf8(ssegs[i]);
            }
            const fs::path snorm = scur.lexically_normal();
            if (!isUnderRoot(sroot, snorm)) {
                if (error) *error = "[escape] AppData 解析结果不在 --appdata 之下";
                return false;
            }
            if (host_path) *host_path = pathToUtf8(snorm);
            return true;
        }

        // ★ ④ 相对路径（相对 guest cwd；缺省 = guest 根）。
        if (!splitGuest(*this, guest, &segs, &err)) {
            if (error) *error = err;
            return false;
        }
    }

    const fs::path root = pathFromUtf8(game_dir_);
    fs::path cur = root;
    for (std::size_t i = 0; i < segs.size(); ++i) {
        const bool last = (i + 1 == segs.size());
        // `resolve_case == false` 只影响**最后一段**（父目录仍要做大小写匹配）
        const bool exact_only = last && !resolve_case;
        std::string real;
        const SegLookup r = lookupSegment(cur, segs[i], exact_only, &real, &err);
        if (r == SegLookup::Error) {
            if (error) *error = err;
            return false;
        }
        if (r == SegLookup::Found) {
            cur /= pathFromUtf8(real);
            continue;
        }
        // 找不到：只有"最后一段 + 允许不存在"才保留调用方拼写（创建新文件用）
        if (!last || must_exist) {
            if (error) {
                // ★ tag 分开：**中间**段缺失 = 目录部分不存在 ⇒ shim 映射成 ERROR_PATH_NOT_FOUND(3)；
                //   **最后**一段缺失 = 文件不存在 ⇒ ERROR_FILE_NOT_FOUND(2)。与 Win32 的区分一致。
                *error = std::string(last ? "[notfound]" : "[notdir]") + " 段 \"" + segs[i] + "\" 在 \"" +
                         pathToUtf8(cur) + "\" 里不存在";
            }
            return false;
        }
        cur /= pathFromUtf8(segs[i]);
    }

    const fs::path normalized = cur.lexically_normal();
    if (!isUnderRoot(root.lexically_normal(), normalized)) {
        if (error) *error = "[escape] 解析结果 \"" + pathToUtf8(normalized) + "\" 不在 game_dir 之下";
        return false;
    }
    if (!normalized.is_absolute()) {
        if (error) *error = "[invalid] 解析结果不是宿主绝对路径";
        return false;
    }
    if (host_path) *host_path = pathToUtf8(normalized);
    return true;
}

bool Vfs::toGuest(const std::string& host_path, std::string* guest_path) const {
    if (guest_path) guest_path->clear();
    if (!ready() || host_path.empty()) return false;
    const fs::path p0 = pathFromUtf8(host_path).lexically_normal();
    // ★ overlay 的反解：宿主上那份拆壳件在 guest 眼里就是 `…\AGE.EXE`
    if (!overlay_host_.empty()) {
        const fs::path oh = pathFromUtf8(overlay_host_).lexically_normal();
        if (oh == p0) {
            if (guest_path) *guest_path = overlay_guest_;
            return true;
        }
    }
    if (!app_data_dir_.empty()) {
        for (const SpecialRoot& sr : specialRoots()) {
            const fs::path sroot = (pathFromUtf8(app_data_dir_) / pathFromUtf8(sr.sub)).lexically_normal();
            if (!isUnderRoot(sroot, p0)) continue;
            std::string rel;
            auto q = p0.begin();
            for (auto r = sroot.begin(); r != sroot.end(); ++r) ++q;
            for (; q != p0.end(); ++q) {
                const std::string part = pathToUtf8(*q);
                if (part.empty()) continue;
                if (!rel.empty()) rel += '\\';
                rel += part;
            }
            if (guest_path) *guest_path = rel.empty() ? std::string(sr.guest)
                                                      : (std::string(sr.guest) + "\\" + rel);
            return true;
        }
    }
    const fs::path root = pathFromUtf8(game_dir_).lexically_normal();
    const fs::path p = pathFromUtf8(host_path).lexically_normal();
    if (!isUnderRoot(root, p)) return false;

    std::string rel;
    auto q = p.begin();
    for (auto r = root.begin(); r != root.end(); ++r) ++q;  // 跳过根前缀（已比过）
    for (; q != p.end(); ++q) {
        const std::string part = pathToUtf8(*q);
        if (part.empty()) continue;
        if (!rel.empty()) rel += '\\';
        rel += part;
    }
    const std::string base = guestRoot().empty() ? std::string("C:\\") : guestRoot();
    if (guest_path) *guest_path = rel.empty() ? base : (trimTrailingSeps(base) + "\\" + rel);
    return true;
}

bool Vfs::find(const std::string& guest_pattern, std::vector<HostDirEntry>* out,
               std::string* error) const {
    if (out) out->clear();
    if (error) error->clear();
    if (!ready()) {
        if (error) *error = "[ready] game_dir 未设置（--game-dir 是唯一根）";
        return false;
    }
    // ★ 入口归一（规则见 vfs.h）：A 版字节（CP932）⇒ UTF-8；日志也用它 ⇒ 不会把 CP932 打进 stderr。
    const std::string shown_pattern = guestPathForLog(guest_pattern);
    const std::string pat = normalizeSeps(shown_pattern);
    if (pat.empty()) {
        if (error) *error = "[invalid] 空的通配符模式";
        return false;
    }
    const std::size_t slash = pat.find_last_of('/');
    const std::string dir_part = (slash == std::string::npos) ? std::string() : pat.substr(0, slash);
    const std::string last = (slash == std::string::npos) ? pat : pat.substr(slash + 1);
    if (last.empty()) {
        if (error) *error = "[invalid] 通配符模式 \"" + shown_pattern + "\" 以路径分隔符结尾";
        return false;
    }
    if (last == "." || last == "..") {
        if (error) *error = "[invalid] 通配符模式段不能是 \".\" / \"..\"（\"" + shown_pattern + "\"）";
        return false;
    }
    if (last.find("**") != std::string::npos) {
        if (error) *error = "[invalid] 不支持 `**`（只实现 `*` / `?` / `*.*`）：\"" + shown_pattern + "\"";
        return false;
    }

    // 目录部分必须存在（相对路径相对 guest cwd）
    const std::string dir_guest = dir_part.empty() ? cwdOf(*this) : dir_part;
    std::string dir_host;
    std::string err;
    if (!resolve(dir_guest, &dir_host, &err, /*must_exist=*/true, /*resolve_case=*/true)) {
        if (error) *error = err;
        return false;
    }
    const fs::path dir = pathFromUtf8(dir_host);
    std::error_code ec;
    if (!fs::is_directory(dir, ec) || ec) {
        if (error) *error = "[notdir] \"" + dir_host + "\" 不是目录";
        return false;
    }

    fs::directory_iterator it(dir, ec);
    if (ec) {
        if (error) *error = "[io] 枚举 \"" + dir_host + "\" 失败：" + ec.message();
        return false;
    }
    // ★ overlay：枚举到被覆盖的那个文件时，size / 时间 / 属性必须取**真正在跑的那份字节**
    //   （否则 `FindFirstFileA` 与 `GetFileSize` 给出两个答案，引擎的版本 / 大小判断前后矛盾）。
    const std::string overlay_name = hasExeOverlay() ? baseName(overlayGuestPath()) : std::string();
    auto isOverlayEntry = [&](const std::string& name) {
        if (overlay_name.empty() || !asciiEqualNoCase(name, overlay_name)) return false;
        return matchesOverlay(absoluteGuest(dir_guest) + "\\" + name);
    };
    auto fillFromOverlayHost = [this](HostDirEntry* e) {
        e->size = 0;
        e->attributes = 0;
        const fs::path hp = pathFromUtf8(overlayHostPath());
        std::error_code c;
        const auto sz = fs::file_size(hp, c);
        if (!c) e->size = static_cast<std::uint64_t>(sz);
        c.clear();
        const fs::file_status st = fs::status(hp, c);
        if (!c) e->attributes = attributesOfStatus(st);
        c.clear();
        HostFileTimes t;
        if (hostFileTimes(overlayHostPath(), &t)) {
            e->create_time_100ns = t.create_100ns;
            e->access_time_100ns = t.access_100ns;
            e->write_time_100ns = t.write_100ns;
        } else {
            const auto wt = fs::last_write_time(hp, c);
            if (!c) {
                const auto tt = static_cast<std::uint64_t>(fileTime100ns(wt));
                e->create_time_100ns = tt;
                e->access_time_100ns = tt;
                e->write_time_100ns = tt;
            }
        }
    };
    bool overlay_seen = false;
    const fs::directory_iterator end;
    while (it != end) {
        const fs::path& p = it->path();
        const std::string name = pathToUtf8(p.filename());
        if (!wildcardMatch(last, name)) {
            it.increment(ec);
            if (ec) {
                if (error) *error = "[io] 枚举 \"" + dir_host + "\" 中断：" + ec.message();
                return false;
            }
            continue;
        }
        std::error_code ec2;
        HostDirEntry e;
        e.name = name;
        e.is_dir = it->is_directory(ec2) && !ec2;
        ec2.clear();
        e.is_file = it->is_regular_file(ec2) && !ec2;
        if (!e.is_dir && !e.is_file) {  // 只返回文件 / 目录（FIFO / 设备 / 断链不返回）
            it.increment(ec);
            if (ec) {
                if (error) *error = "[io] 枚举 \"" + dir_host + "\" 中断：" + ec.message();
                return false;
            }
            continue;
        }
        if (isOverlayEntry(name)) {
            e.is_dir = false;
            e.is_file = true;
            fillFromOverlayHost(&e);
            overlay_seen = true;
            if (out) out->push_back(std::move(e));
            it.increment(ec);
            if (ec) {
                if (error) *error = "[io] 枚举 \"" + dir_host + "\" 中断：" + ec.message();
                return false;
            }
            continue;
        }
        if (e.is_file) {
            std::error_code ec3;
            const auto sz = it->file_size(ec3);
            if (!ec3) e.size = static_cast<std::uint64_t>(sz);
        }
        std::error_code ec4;
        const fs::file_status st = it->status(ec4);
        if (!ec4) e.attributes = attributesOfStatus(st);
        ec4.clear();
        // 三个时间取宿主真值（SDL3）；失败才退回 mtime 近似（三值相同）
        HostFileTimes times;
        if (hostFileTimes(pathToUtf8(p), &times)) {
            e.create_time_100ns = times.create_100ns;
            e.access_time_100ns = times.access_100ns;
            e.write_time_100ns = times.write_100ns;
        } else {
            const auto wt = it->last_write_time(ec4);
            if (!ec4) {
                const auto t = static_cast<std::uint64_t>(fileTime100ns(wt));
                e.create_time_100ns = t;
                e.access_time_100ns = t;
                e.write_time_100ns = t;
            }
        }
        if (out) out->push_back(std::move(e));

        it.increment(ec);
        if (ec) {
            if (error) *error = "[io] 枚举 \"" + dir_host + "\" 中断：" + ec.message();
            return false;
        }
    }

    // ★ overlay：`--game-dir` 下**没有**这个文件时（只有 overlay 那一份存在），枚举仍要能看见它
    //   （否则 `FindFirstFileA("…\*.EXE")` 与 `CreateFileA("…\AGE.EXE")` 会给出相反的答案）。
    if (!overlay_seen && !overlay_name.empty() && wildcardMatch(last, overlay_name) &&
        matchesOverlay(absoluteGuest(dir_guest) + "\\" + overlay_name) && out) {
        HostDirEntry e;
        e.name = overlay_name;
        e.is_dir = false;
        e.is_file = true;
        fillFromOverlayHost(&e);
        out->push_back(std::move(e));
    }

    // ★ 确定性：永远排序后返回（UTF-8 字节序）；⛔ 不依赖宿主的目录顺序
    if (out) {
        std::sort(out->begin(), out->end(),
                  [](const HostDirEntry& a, const HostDirEntry& b) { return a.name < b.name; });
    }
    return true;
}

bool Vfs::isDirectory(const std::string& guest_path) const {
    std::string host;
    std::string err;
    if (!resolve(guest_path, &host, &err, /*must_exist=*/true, /*resolve_case=*/true)) return false;
    std::error_code ec;
    return fs::is_directory(pathFromUtf8(host), ec) && !ec;
}

bool Vfs::isFile(const std::string& guest_path) const {
    std::string host;
    std::string err;
    if (!resolve(guest_path, &host, &err, /*must_exist=*/true, /*resolve_case=*/true)) return false;
    std::error_code ec;
    return fs::is_regular_file(pathFromUtf8(host), ec) && !ec;
}

std::string Vfs::absoluteGuest(const std::string& guest_path) const {
    if (guest_path.empty()) return guestRoot();
    std::vector<std::string> segs;
    std::string err;
    if (!splitGuest(*this, guest_path, &segs, &err)) {
        // 逃出根 / 非法：原样返回（调用方的 resolve 会给出明确失败，⛔ 不在这里悄悄改写成根）
        return guest_path;
    }
    std::string out = guestRoot();
    if (out.empty()) out = "C:";
    for (const std::string& s : segs) {
        out += '\\';
        out += s;
    }
    return out;
}

// ★ 这两个是给日志 / 诊断用的**文本**函数：保持调用方给的隔离符（`\` 仍写 `\`），
//   只去掉尾部隔离符；⛔ 不在这里做规范化（那会让日志里出现调用方没写过的路径）。
std::string Vfs::canonicalGuest(const std::string& guest_path) const {
    if (guest_path.empty()) return std::string();
    std::vector<std::string> segs;
    std::string err;
    // clamp_escape：`GetFullPathName` 是**文本**规范化 ⇒ `..` 在根上就停在根，不报错
    splitGuestImpl(*this, guest_path, &segs, &err, /*clamp_escape=*/true);
    std::string out = guestRoot().empty() ? std::string("C:") : guestRoot();
    while (out.size() > 1 && (out.back() == '\\' || out.back() == '/')) out.pop_back();
    if (out.size() == 2 && out[1] == ':') out += '\\';  // "C:" → "C:\"（根目录形态）
    for (const std::string& s : segs) {
        if (out.back() != '\\') out += '\\';
        out += s;
    }
    return out;
}

std::string Vfs::parentGuest(const std::string& guest_path) {
    std::string s = guest_path;
    while (s.size() > 1 && (s.back() == '\\' || s.back() == '/')) s.pop_back();
    const std::size_t pos = s.find_last_of("\\/");
    if (pos == std::string::npos) return std::string();
    if (pos == 0) return std::string();                  // "\\foo" ⇒ 没有父目录可回
    if (hasDrive(s) && pos == 2) return s.substr(0, 3);  // "E:\\foo" ⇒ "E:\\"
    return s.substr(0, pos);
}

std::string Vfs::baseName(const std::string& guest_path) {
    std::string s = guest_path;
    while (s.size() > 1 && (s.back() == '\\' || s.back() == '/')) s.pop_back();
    const std::size_t pos = s.find_last_of("\\/");
    return pos == std::string::npos ? s : s.substr(pos + 1);
}

// =======================================================================================
// ★ P4：`imagehlp!MakeSureDirectoryPathExists`（递归建目录）
//
// 语义（Win32 的文档口径，`.` 见 imagehlp.cpp 文件头）：
//   · "creates all the directories in the specified DirPath, beginning with the root"；
//     每个分隔符（`\` 或 `/`）**之前**的那一段都是目录；**末尾分隔符**把最后一段也标成目录；
//     没有末尾分隔符 ⇒ 最后一段是**文件名**，⛔ 不建它（文档明说：最后一段是目录时必须以分隔符结尾）。
//   · **已存在算成功**（与 CreateDirectoryA 的"已存在即失败"相反）—— 这是真机语义。
//   · 只经本类落盘 ⇒ guest 看到的仍是 Windows 形状，宿主路径不泄漏。
//
// ★ 为什么前缀要过"是不是根"这一关：`E:\Games\Eushully\天結\save\` 的前缀依次是
//   `E:` / `E:\Games` / `E:\Games\Eushully` / … / `…\天結` / `…\天結\save`。前几个**在 guest 世界里
//   本来就存在**（它们是 guest 根的一段），而 `resolve` 只能看到"guest 根之下的段"⇒ 前缀
//   `E:\Games` 会被解析成 `game_dir/Games`。不拦的话第一次启动就会在安装树旁边造出一棵
//   `Games/Eushully/天結` 假树（实测过：`--game-dir` 是唯一根，那棵树**不在**任何真机上存在）。
//   ⇒ 凡是"等于根 / 是根的祖先"的前缀一律跳过（根不创建）。
// =======================================================================================
bool Vfs::makeSureDirectoryPathExists(const std::string& guest_path, std::string* error) const {
    if (error) error->clear();
    if (!ready()) {
        if (error) *error = "[ready] game_dir 未设置（--game-dir 是唯一根）";
        return false;
    }
    if (guest_path.empty()) {
        if (error) *error = "[invalid] 空路径";
        return false;
    }

    // ★ `--appdata` 的映射脚手架（`<appdata>/Roaming` / `Local` / `Documents`）是**宿主侧**的形态
    //   （plan D5：形态由实现固定）。`resolve()` 只解析、不建目录，而它要求脚手架已存在 ⇒
    //   这里先把那条映射目录建出来（否则"首次用一个新的 --appdata 就写不进 AppData"）。
    const std::string special_text = normalizeSeps(guest_path);
    for (const SpecialRoot& sr : specialRoots()) {
        if (!prefixIsDir(special_text, sr.guest)) continue;
        if (app_data_dir_.empty()) {
            if (error) {
                *error = std::string("[appdata] 特殊目录 \"") + sr.guest + "\" 需要 --appdata，但当前为空";
            }
            return false;
        }
        const fs::path scaffold = (pathFromUtf8(app_data_dir_) / pathFromUtf8(sr.sub)).lexically_normal();
        std::error_code sec;
        if (!fs::is_directory(scaffold, sec)) {
            sec.clear();
            if (!fs::create_directories(scaffold, sec) || sec) {
                if (error) {
                    *error = "AppData 映射目录建不出来：" + (sec ? sec.message() : std::string("宿主拒绝"));
                }
                return false;
            }
        }
        break;
    }

    // 比较用的规范形：`\` → `/`、去盘符、去前导 / 尾部分隔符（"E:\Games" ⇒ "Games"）。
    auto canon = [](std::string s) {
        s = normalizeSeps(std::move(s));
        if (hasDrive(s)) s.erase(0, 2);
        trimLeadingSeps(s);
        return trimTrailingSeps(std::move(s));
    };
    // "根"的集合：guest 根 + --appdata 的三个特殊目录（它们都**已经存在**）。
    std::vector<std::string> roots;
    roots.push_back(canon(guestRoot()));
    for (const SpecialRoot& sr : specialRoots()) roots.push_back(canon(sr.guest));

    auto atOrAboveRoot = [&](const std::string& raw) {
        const std::string p = canon(raw);
        if (p.empty()) return true;  // 只有盘符 / 只有分隔符 ⇒ 根
        for (const std::string& r : roots) {
            if (r.empty()) continue;
            if (asciiEqualNoCase(r, p)) return true;
            if (r.size() > p.size() && r.compare(0, p.size(), p) == 0 && r[p.size()] == '/') return true;
        }
        return false;
    };

    // 盘符不是目录（`C:` 由 splitGuest 统一剥掉）
    const std::size_t start = hasDrive(guest_path) ? 2u : 0u;
    for (std::size_t i = start; i < guest_path.size(); ++i) {
        const char c = guest_path[i];
        if (c != '\\' && c != '/') continue;
        const std::string dir = guest_path.substr(0, i);
        if (atOrAboveRoot(dir)) continue;  // 根（或根的祖先）不创建
        std::string host;
        std::string why;
        if (!resolve(dir, &host, &why, /*must_exist=*/false, /*resolve_case=*/true)) {
            if (error) *error = "段 \"" + dir + "\" 解析失败：" + why;
            return false;
        }
        const fs::path hp = pathFromUtf8(host);
        std::error_code ec;
        if (fs::is_directory(hp, ec) && !ec) continue;  // ★ 已存在算成功
        ec.clear();
        // ★ 用 create_directories（不是 create_directory）：宿主侧的**映射脚手架**不属于 guest 路径
        //   —— `--appdata` 下的 `Roaming` / `Local` / `Documents` 是本模拟器的落点形态（plan D5），
        //   guest 只写 `C:\Users\Amayui\AppData\Roaming\…`，那条路径里根本没有 `Roaming` 这一段。
        //   ⇒ 首次写 AppData 时必须能把脚手架一起建出来，否则引擎第一次存档 / 写配置就失败。
        if (!fs::create_directories(hp, ec) || ec) {
            if (error) {
                *error = "建目录 \"" + dir + "\" 失败：" +
                         (ec ? ec.message() : std::string("宿主拒绝"));
            }
            return false;
        }
    }
    return true;
}

}  // namespace lift
