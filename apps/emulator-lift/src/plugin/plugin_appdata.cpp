// emulator-lift · `ctx.appdata` 的宿主实现（只读 stat；口径见头注释）
#include "plugin/plugin_appdata.h"

#include <cstdint>
#include <filesystem>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <string.h>  // _wcsicmp
#endif

#include "runtime/host_encoding.h"

namespace lift::plugin {

namespace {

namespace fs = std::filesystem;

// ★ 本文件是第三个需要"UTF-8 文本 ↔ `fs::path`"的地方（前两处都在 `vfs.cpp` 的匿名 namespace）。
//   复用 `host_encoding.h` 的两个底层转换，而不是再写一份多字节解码表。
fs::path pathFromUtf8(const std::string& text) {
#ifdef _WIN32
    return fs::path(lift::utf8ToWide(text));
#else
    return fs::path(text);
#endif
}

std::string pathToUtf8(const fs::path& p) {
#ifdef _WIN32
    return lift::wideToUtf8(p.wstring().c_str());
#else
    return p.string();
#endif
}

bool sameComponent(const fs::path& a, const fs::path& b) {
#ifdef _WIN32
    return ::_wcsicmp(a.c_str(), b.c_str()) == 0;
#else
    return a == b;
#endif
}

// `p` 在 `root` 之下（含相等）。逐段比较：Windows 大小写不敏感，大小写只在宿主语义里才有意义。
bool isUnder(const fs::path& root, const fs::path& p) {
    auto r = root.begin();
    auto q = p.begin();
    for (; r != root.end(); ++r, ++q) {
        if (q == p.end()) return false;
        if (!sameComponent(*r, *q)) return false;
    }
    return true;
}

// 判"不存在"时再区分一次 `not_dir`：路径前缀里有一段是**文件**。
// ★ 为什么不能只靠 `std::filesystem`：Windows 的 `weakly_canonical` / `status` 对
//   `file.txt/child` 只报 ENOENT（判不出"中间段不是目录"）⇒ 逐段 stat 一次（只在"缺失"路径上）。
AppDataStatError classifyMissing(const fs::path& root, const std::vector<std::string>& segs) {
    fs::path dir = root;
    for (std::size_t i = 0; i + 1 < segs.size(); ++i) {
        dir /= pathFromUtf8(segs[i]);
        std::error_code ec;
        const fs::file_status st = fs::status(dir, ec);
        if (ec) return AppDataStatError::NotFound;
        if (fs::exists(st) && !fs::is_directory(st)) return AppDataStatError::NotDir;
    }
    return AppDataStatError::NotFound;
}

AppDataStatError mapError(const std::error_code& ec) {
    if (!ec) return AppDataStatError::None;
    if (ec == std::errc::permission_denied) return AppDataStatError::Denied;
    if (ec == std::errc::no_such_file_or_directory) return AppDataStatError::NotFound;
    if (ec == std::errc::not_a_directory) return AppDataStatError::NotDir;
    return AppDataStatError::Io;
}

// 把插件给的相对路径拆成段；拒绝空串 / 控制字符 / 绝对路径 / `..`。
bool splitRelative(const std::string& rel, std::vector<std::string>* segs) {
    if (rel.empty()) return false;
    for (const unsigned char c : rel) {
        if (c == 0 || c < 0x20 || c == 0x7f) return false;  // NUL / 控制字符（std::filesystem 会截断）
    }
    const fs::path raw = pathFromUtf8(rel);
    if (raw.is_absolute() || raw.has_root_name() || raw.has_root_directory()) return false;
    std::size_t start = 0;
    while (start <= rel.size()) {
        const std::size_t sep = rel.find_first_of("/\\", start);
        const std::size_t end = sep == std::string::npos ? rel.size() : sep;
        const std::string seg = rel.substr(start, end - start);
        if (!seg.empty() && seg != ".") {
            if (seg == "..") return false;  // ⛔ 先拒绝，不做"归一化后再判"（大小写 / 短名上会漏）
            segs->push_back(seg);
        }
        if (sep == std::string::npos) break;
        start = sep + 1;
    }
    return !segs->empty();
}

}  // namespace

bool PluginAppData::attach(const std::string& appdata_dir, const std::string& install_dir,
                           std::string* err) {
    available_ = false;
    path_.clear();
    canon_.clear();
    if (appdata_dir.empty()) return true;  // 没有 --appdata：正常的"不可用"，不是错误
    std::error_code ec;
    fs::path root = fs::absolute(pathFromUtf8(appdata_dir), ec);
    if (ec || root.empty()) {
        if (err != nullptr) *err = "--appdata 无法绝对化（" + ec.message() + "）";
        return false;
    }
    root = root.lexically_normal();
    if (root != root.root_path() && !root.has_filename()) root = root.parent_path();
    if (!install_dir.empty()) {
        std::error_code iec;
        fs::path install = fs::absolute(pathFromUtf8(install_dir), iec);
        if (!iec) {
            install = install.lexically_normal();
            if (isUnder(install, root)) {
                if (err != nullptr) {
                    *err = "--appdata 落在 --game-dir（只读安装树）里 ⇒ 不把它当 appdata 根";
                }
                return false;
            }
        }
    }
    std::error_code cec;
    fs::path canonical = fs::weakly_canonical(root, cec);
    if (cec) canonical = root;  // 根不可读时退回绝对化结果；真正的拒绝留给 escape / denied
    path_ = pathToUtf8(root);
    canon_ = pathToUtf8(canonical);
    available_ = true;
    return true;
}

AppDataStat PluginAppData::stat(const std::string& rel) const {
    AppDataStat out;
    if (!available_) {
        out.error = AppDataStatError::NoAppData;
        return out;
    }
    std::vector<std::string> segs;
    if (!splitRelative(rel, &segs)) {
        out.error = AppDataStatError::InvalidPath;
        return out;
    }
    const fs::path root = pathFromUtf8(canon_);
    fs::path target = root;
    for (const std::string& seg : segs) target /= pathFromUtf8(seg);
    std::error_code ec;
    const fs::path canonical = fs::weakly_canonical(target, ec);
    if (ec) {
        const AppDataStatError mapped = mapError(ec);
        out.error = mapped == AppDataStatError::NotFound ? classifyMissing(root, segs) : mapped;
        return out;
    }
    if (!isUnder(root, canonical)) {
        out.error = AppDataStatError::Escape;
        return out;
    }
    const fs::file_status st = fs::status(canonical, ec);
    if (ec) {
        const AppDataStatError mapped = mapError(ec);
        out.error = mapped == AppDataStatError::NotFound ? classifyMissing(root, segs) : mapped;
        return out;
    }
    if (!fs::exists(st)) {
        out.error = classifyMissing(root, segs);
        return out;
    }
    out.exists = true;
    if (fs::is_directory(st)) {
        out.is_dir = true;
    } else if (fs::is_regular_file(st)) {
        out.is_file = true;
        std::error_code sec;
        const std::uintmax_t size = fs::file_size(canonical, sec);
        if (!sec) {
            out.size = static_cast<std::uint64_t>(size);
            out.has_size = true;
        }
    }
    return out;
}

}  // namespace lift::plugin
