#include "plugin/plugin_search.h"

#include <filesystem>
#include <system_error>

#include "runtime/host_encoding.h"

namespace lift::plugin {
namespace {

namespace fs = std::filesystem;

/** UTF-8 串 → 宿主 `fs::path`（Windows 上必须走宽字符：窄构造按 ANSI 码页解释，日文路径会造错名字）。 */
fs::path pathFromUtf8(const std::string& text) {
#ifdef _WIN32
    return fs::path(lift::utf8ToWide(text));
#else
    return fs::path(text);
#endif
}

/** 宿主 `fs::path` → UTF-8 串（`fs::path::string()` 在 Windows 上会退到 ANSI 码页）。 */
std::string utf8FromPath(const fs::path& path) {
#ifdef _WIN32
    return lift::wideToUtf8(path.wstring().c_str());
#else
    return path.string();
#endif
}

/**
 * 拼一个候选路径：`<root>[/scripts/plugins]/<file>`。
 * ★ 用 `fs::path` 的 `operator/=` 拼接（而不是字符串加 `/`）⇒ Windows 上得到**原生分隔符**，
 *   日志 / 错误文案里的路径与 `fs::path` 的其它用法同形；跟不跟随根末尾的分隔符都由它处理。
 */
fs::path candidatePath(const std::string& root, const std::string& file, bool under_scripts_plugins) {
    fs::path path = pathFromUtf8(root);
    if (under_scripts_plugins) {
        path /= "scripts";
        path /= "plugins";
    }
    path /= pathFromUtf8(file);
    return path;
}

/** 绝对化（失败 ⇒ 原样返回；⛔ 不抛异常 —— 解析器不该把进程带崩）。 */
std::string absoluteOf(const fs::path& path) {
    std::error_code ec;
    const fs::path absolute = fs::absolute(path, ec);
    if (ec) return utf8FromPath(path);
    return utf8FromPath(absolute);
}

/** 命中判据 = **常规文件**（目录 / 特殊文件都不算；跟随符号链接 = `status()` 的缺省行为）。 */
bool isRegularFile(const std::string& text) {
    std::error_code ec;
    const bool regular = fs::is_regular_file(pathFromUtf8(text), ec);
    return !ec && regular;
}

bool isIdChar(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '-';
}

}  // namespace

bool isPluginId(const std::string& spec) {
    // ★ 长度 1..64，字符集只有 ASCII 字母 / 数字 / `_` / `-`。
    //   空串 ⇒ false ⇒ 走路径（`--plugin ""` 的失败面交给 `initFromFile`，与今天同形）。
    if (spec.empty() || spec.size() > 64) return false;
    for (const char c : spec) {
        if (!isIdChar(c)) return false;
    }
    return true;
}

PluginResolveResult resolvePluginSpec(const std::string& spec, const PluginResolveRoots& roots) {
    PluginResolveResult result;
    result.spec = spec;
    if (!isPluginId(spec)) {
        // ★ 路径形式：**原样**（cwd 相对 / 绝对 / `.js` / 空串），⛔ 不绝对化、⛔ 不早检查存在性
        //   （那会把"装载失败 exit 2"的文案换成另一句，等于改行为）。
        result.kind = PluginSpecKind::Path;
        result.path = spec;
        result.status = PluginResolveResult::Status::Ok;
        return result;
    }

    result.kind = PluginSpecKind::Id;
    result.id = spec;
    const std::string file_name = spec + ".js";
    // ★ 顺序 = 发行优先、开发回退（与 `--overlay` 的 hud 解析同一条口径）；⛔ 不查 `lib/`、⛔ 不递归。
    if (!roots.exe_dir.empty()) {
        result.searched.push_back(absoluteOf(candidatePath(roots.exe_dir, file_name, true)));
    }
    if (!roots.dev_dir.empty()) {
        result.searched.push_back(absoluteOf(candidatePath(roots.dev_dir, file_name, false)));
    }
    for (const std::string& candidate : result.searched) {
        if (isRegularFile(candidate)) result.hits.push_back(candidate);
    }
    if (result.hits.size() == 1) {
        result.path = result.hits.front();
        result.status = PluginResolveResult::Status::Ok;
    } else if (result.hits.empty()) {
        result.status = PluginResolveResult::Status::NotFound;
    } else {
        result.status = PluginResolveResult::Status::Ambiguous;
    }
    return result;
}

}  // namespace lift::plugin
