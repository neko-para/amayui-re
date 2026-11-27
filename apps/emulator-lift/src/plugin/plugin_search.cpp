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

/** ASCII 小写（⛔ 不用 locale / `std::tolower`：判据必须与平台 locale 无关）。 */
char asciiLower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

}  // namespace

bool isReservedHudId(const std::string& spec) {
    // ★ **ASCII 大小写不敏感**（S2 复审 minor 6）：Windows 卷不敏感 ⇒ `--plugin HUD` 若按精确串
    //   判就会走普通根（把用户根里的 `hud.js` 当普通 id 装上，**绕过** ReservedShadowed）。
    if (spec.size() != 3) return false;
    return asciiLower(spec[0]) == 'h' && asciiLower(spec[1]) == 'u' && asciiLower(spec[2]) == 'd';
}

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
        //   ★ 保留 id 的语义**只对 id 形式**生效：`--plugin <...>/hud.js` 仍是普通路径。
        result.kind = PluginSpecKind::Path;
        result.path = spec;
        result.status = PluginResolveResult::Status::Ok;
        return result;
    }

    result.kind = PluginSpecKind::Id;
    result.id = spec;
    // ★ 两个根**可能指向同一路径**（同一目录 / junction / 把 exe 放进源码 plugins 目录）⇒ 候选去重：
    //   同一份文件不许被报成歧义（用户口径的"两根同名 ⇒ exit 2"指的是**两份不同文件**）。
    const auto push_candidate = [&result](const std::string& candidate) {
        for (const std::string& seen : result.searched) {
            if (seen == candidate) return;
        }
        result.searched.push_back(candidate);
    };
    // ★ 命中再按**文件身份**去重（canonical 相同 = 同一份文件：符号链接 / junction / 大小写别名）：
    //   报错文案里的"N 处"必须是 N 份**不同**文件。canonical 取不到（权限 / 不存在）⇒ 退回候选原串。
    const auto collect_hits = [&result]() {
        std::vector<fs::path> identities;
        for (const std::string& candidate : result.searched) {
            if (!isRegularFile(candidate)) continue;
            std::error_code ec;
            fs::path identity = fs::weakly_canonical(pathFromUtf8(candidate), ec);
            if (ec) identity = pathFromUtf8(candidate);
            bool duplicate = false;
            for (const fs::path& seen : identities) {
                if (seen == identity) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) continue;
            identities.push_back(identity);
            result.hits.push_back(candidate);
        }
    };

    // ---- S2：保留 id `hud`（`--overlay` 预设与 `--plugin hud` 走同一解析）----
    //   ① 用户插件根里的 `hud.js` = **遮蔽**（exit 2；⛔ 不静默换源 / 不静默被内置件遮住）；
    //   ② 之后才是保留根（发行 → 开发回退），顺序与旧 `--overlay` 完全一致；
    //   ③ 普通根的候选**不进** `searched` 的"找不到"清单（那一层不参与保留 id 的解析）。
    if (isReservedHudId(spec)) {
        // ★ S1b 裁决：`hud` 的遮蔽检查**保持只看 `.js`**。保留 id 解析的是内置 `scripts/ui/hud.js`
        //   （发行件）；`.ts` HUD 不在本轮范围，让它参与遮蔽只会把 `--overlay` 变成一次静默换源
        //   （而且没有"内置 `.ts`"可回退）⇒ 用户根里的 `hud.ts` 不触发 `ReservedShadowed`。
        const std::string shadow_name = std::string(kReservedHudId) + ".js";
        if (!roots.exe_dir.empty()) {
            push_candidate(absoluteOf(candidatePath(roots.exe_dir, shadow_name, true)));
        }
        if (!roots.dev_dir.empty()) {
            push_candidate(absoluteOf(candidatePath(roots.dev_dir, shadow_name, false)));
        }
        collect_hits();
        if (!result.hits.empty()) {
            result.status = PluginResolveResult::Status::ReservedShadowed;
            return result;
        }
        // 遮蔽检查的候选已用完 ⇒ 换成保留根的候选（`searched` 是 NotFound 的唯一文案来源）。
        result.searched.clear();
        if (!roots.exe_dir.empty()) {
            // `<exeDir>scripts/ui/hud.js`（发行布局；与 `--overlay` 旧解析同形）。
            fs::path hud_exe = pathFromUtf8(roots.exe_dir);
            hud_exe /= "scripts";
            hud_exe /= "ui";
            hud_exe /= pathFromUtf8(shadow_name);
            push_candidate(absoluteOf(hud_exe));
        }
        if (!roots.hud_dev_dir.empty()) {
            push_candidate(absoluteOf(candidatePath(roots.hud_dev_dir, shadow_name, false)));
        }
        collect_hits();
        // ★ 保留 id 用**优先序**（发行 → 开发回退），与 S2 之前的 `--overlay` 逐字同一条：
        //   两份都在场时 exe 那份赢（⛔ 不报歧义 —— 那会把"发行优先"变成启动失败）。
        if (!result.hits.empty()) {
            result.path = result.hits.front();
            result.status = PluginResolveResult::Status::Ok;
        } else {
            result.status = PluginResolveResult::Status::NotFound;
        }
        return result;
    }

    // ★ S1b：id 的候选扩展名 = `.js` → `.ts` → `.mts`（**扩展名优先于根**）。
    //   裁决：同一 id 同时有 `.js` / `.ts` ⇒ **`.js` 优先**（`.js` 是历史行为、也是发行件；
    //   `.ts` 是新增能力；⛔ 不静默换源，也⛔ 不报歧义 —— 没有任何读数能判"用户要哪一个"）。
    //   命中第一档就停；前面 0 命中的候选仍进 `searched`（NotFound 的文案要列全试过哪些）。
    // ⛔ 不查 `lib/`、⛔ 不递归（与之前同一条）。
    const char* const kExtensions[] = {".js", ".ts", ".mts"};
    for (const char* extension : kExtensions) {
        const std::string file_name = spec + extension;
        if (!roots.exe_dir.empty()) {
            push_candidate(absoluteOf(candidatePath(roots.exe_dir, file_name, true)));
        }
        if (!roots.dev_dir.empty()) {
            push_candidate(absoluteOf(candidatePath(roots.dev_dir, file_name, false)));
        }
        const std::size_t before = result.hits.size();
        collect_hits();
        if (result.hits.size() == before) continue;  // 本扩展名 0 命中 ⇒ 试下一个
        break;                                       // 第一档命中：`.js` 的优先级由循环顺序决定
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
