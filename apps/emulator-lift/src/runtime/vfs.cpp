// emulator-lift · VFS 实现：Win32 guest 路径 ↔ 宿主路径（唯一根 = `--game-dir`）
//
// ★ 合同在 `vfs.h`（本文件只是它的实现）。口径与风险点（plan §6 S6 / §9 R2）：
//   · **唯一根**：guest 绝对路径先剥盘符 `X:`、再剥 `guestRoot()` 前缀，然后拼到 game_dir；
//     `..` 越出根 ⇒ **失败**（⛔ 不夹回根里）。
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
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

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

// 根前缀的"无盘符"形式集合（`E:\Games\Eushully\天結` → `Games/Eushully/天結`；`天結` 这类
// 不带盘符的根也照样成立）。比较用 ASCII 大小写不敏感。
std::vector<std::string> rootNoDriveForms(const Vfs& v) {
    std::vector<std::string> out;
    std::string raw = trimTrailingSeps(normalizeSeps(v.guestRoot()));
    if (raw.empty()) return out;
    std::string nod = stripDrive(raw);
    trimLeadingSeps(nod);
    if (!nod.empty()) out.push_back(nod);
    return out;
}

// `text` 是否以目录形式 `form` 开头（form 之后必须是 '/' 或结尾）
bool prefixIsDir(std::string_view text, std::string_view form) {
    if (form.empty() || text.size() < form.size()) return false;
    if (!asciiEqualNoCase(text.substr(0, form.size()), form)) return false;
    return text.size() == form.size() || text[form.size()] == '/';
}

bool isAbsoluteText(const Vfs& v, const std::string& text) {
    if (hasDrive(text)) return true;
    if (!text.empty() && text[0] == '/') return true;
    for (const std::string& form : rootNoDriveForms(v)) {
        if (prefixIsDir(text, form)) return true;
    }
    return false;
}

std::string joinGuest(const std::string& dir, const std::string& rel) {
    if (rel.empty()) return dir;
    std::string base = trimTrailingSeps(dir);
    if (base.empty()) return rel;
    return base + "/" + rel;
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
            *error = "[escape] 路径 `" + original + "` 里的 `..` 越出 guest 根（唯一根是 game_dir）";
            return false;
        }
        segs->pop_back();
        return true;
    }
    segs->push_back(seg);
    return true;
}

bool splitGuestImpl(const Vfs& v, const std::string& guest_path, std::vector<std::string>* segs,
                    std::string* error, bool clamp_escape) {
    segs->clear();
    std::string text = normalizeSeps(guest_path);
    if (text.empty()) text = cwdOf(v);
    if (!isAbsoluteText(v, text)) text = joinGuest(cwdOf(v), text);
    // ★ 拼过 cwd 之后必须再规范化一次：`guestRoot()` / `guestCwd()` 的**文本形式用的是 `\`**
    //   （vfs.h 的缺省根是 `E:\Games\Eushully\天結`）⇒ 不重规范化会得到混合隔离符，
    //   与 rootNoDriveForms 的 `/` 形式比不中，整条相对路径会解析到 game_dir/Games/... 下面。
    text = normalizeSeps(text);
    text = stripDrive(text);
    trimLeadingSeps(text);
    for (const std::string& form : rootNoDriveForms(v)) {
        if (prefixIsDir(text, form)) {
            text = text.substr(form.size());
            trimLeadingSeps(text);
            break;
        }
    }

    std::string cur;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '/') {
            if (!applySegment(cur, guest_path, segs, error, clamp_escape)) return false;
            cur.clear();
        } else {
            cur.push_back(text[i]);
        }
    }
    return true;
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
// 宿主文件时间（平台抽象 = SDL3；⛔ 这里没有 `#ifdef` 平台分支）
// =======================================================================================
bool hostFileTimes(const std::string& host_path, HostFileTimes* out) {
    if (!out) return false;
    SDL_PathInfo info{};
    if (!SDL_GetPathInfo(host_path.c_str(), &info)) return false;
    // SDL_Time = 自 1970 起的**纳秒**；`SDL_TimeToWindows` 转成 FILETIME（100ns 自 1601）。
    // ★ 0 表示"这个文件系统不提供该时间" ⇒ 按 Windows 文档留 FILETIME 0，⛔ 不编一个 1970。
    auto toWindows100ns = [](SDL_Time t) -> std::uint64_t {
        if (t <= 0) return 0;
        Uint32 lo = 0;
        Uint32 hi = 0;
        SDL_TimeToWindows(t, &lo, &hi);
        return (static_cast<std::uint64_t>(hi) << 32) | lo;
    };
    out->create_100ns = toWindows100ns(info.create_time);
    out->access_100ns = toWindows100ns(info.access_time);
    out->write_100ns = toWindows100ns(info.modify_time);
    return true;
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
}

void Vfs::setGuestCwd(const std::string& guest_path) {
    guest_cwd_ = absoluteGuest(guest_path);
    guest_cwd_set_ = true;
    // `guest_cwd_set_` 只表达"调用方显式设过"（缺省值口径见 cwdOf）；读一次免得它变成
    // "只写不读的私有字段"（-Wunused-private-field 会响）。
    (void)guest_cwd_set_;
}

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
    std::string err;
    std::vector<std::string> segs;
    if (!splitGuest(*this, guest_path, &segs, &err)) {
        if (error) *error = err;
        return false;
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
    const std::string pat = normalizeSeps(guest_pattern);
    if (pat.empty()) {
        if (error) *error = "[invalid] 空的通配符模式";
        return false;
    }
    const std::size_t slash = pat.find_last_of('/');
    const std::string dir_part = (slash == std::string::npos) ? std::string() : pat.substr(0, slash);
    const std::string last = (slash == std::string::npos) ? pat : pat.substr(slash + 1);
    if (last.empty()) {
        if (error) *error = "[invalid] 通配符模式 \"" + guest_pattern + "\" 以路径分隔符结尾";
        return false;
    }
    if (last == "." || last == "..") {
        if (error) *error = "[invalid] 通配符模式段不能是 \".\" / \"..\"（\"" + guest_pattern + "\"）";
        return false;
    }
    if (last.find("**") != std::string::npos) {
        if (error) *error = "[invalid] 不支持 `**`（只实现 `*` / `?` / `*.*`）：\"" + guest_pattern + "\"";
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

}  // namespace lift
