// emulator-lift · `--plugin <id|path>` **纯解析器**的判据（`plugin-resolve`，S1）
//
// 红了意味着什么（三条都是"改了就是 bug、而且**没有运行期症状**"）：
//   ① id 语法被改松 / 改紧：`.js` 后缀被当成 id ⇒ 今天的 `--plugin x.js` 从"cwd 相对文件"
//      静默变成"白名单根里的同名文件"（**换源**）；`..` 被放进来 ⇒ 搜索面偷偷扩大。
//   ② 搜索根被加宽 / 改顺序：把 `lib/` 也搜、递归子目录、或"第一个命中赢" ⇒
//      用户装了另一份同名文件就会被装上（或歧义被静默吃掉）。
//   ③ 路径形式被绝对化 / 早检查：cwd 相对 / 绝对 / "装载失败 exit 2" 三条行为都会变。
//   ④ **S2 保留 id `hud`** 的解析顺序（发行 → 开发回退）或遮蔽判据被改松：`--overlay` 会静默
//      装上"另一份 hud.js"（用户同名文件胜出 / 内置件遮住用户文件，两种都无运行期症状）。
//
// ★ 判据只碰 `temp_directory_path()` 下的**注入 roots**（临时目录 + 几个 `.js` 文件）：
//   零 exe / 零 GPU / 零资产 / 零游戏安装 ⇒ 顶层无条件注册 = pure 档（`AGENTS.md` §12.4）。
// ★ 断言的是**分类 / 顺序 / 候选清单**，⛔ 不断言任何游戏读数、⛔ 不锚具体路径字符串（除临时根）。
// ★ 大小写那条**运行时探测当前卷**（造两个只差 ASCII 大小写的文件、数目录项），⛔ 不按
//   `_WIN32` / `__APPLE__` 猜平台：Windows NTFS 与 macOS APFS **默认都不敏感**，Linux ext4 敏感，
//   而 APFS 可建成敏感卷 / NTFS 可开 per-directory 大小写（先例：`tests/file_path_ext.cpp:1204`）。
//   ⇒ 断言的是"解析器跟随当前卷的语义"，读数（`[resolve/id-case] 卷大小写…`）进 ctest 输出。
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include "check.h"
#include "plugin/plugin_search.h"
#include "runtime/host_encoding.h"

namespace {

namespace fs = std::filesystem;
using lift::plugin::isPluginId;
using lift::plugin::PluginResolveResult;
using lift::plugin::PluginResolveRoots;
using lift::plugin::PluginSpecKind;
using Status = lift::plugin::PluginResolveResult::Status;

std::uint32_t processId() {
#ifdef _WIN32
    return static_cast<std::uint32_t>(::_getpid());
#else
    return static_cast<std::uint32_t>(::getpid());
#endif
}

std::string toUtf8(const fs::path& path) {
#ifdef _WIN32
    return lift::wideToUtf8(path.wstring().c_str());
#else
    return path.string();
#endif
}

/** 临时树：`<TEMP>/lift-plugin-resolve-<pid>`；析构删掉（里面只有本用例造的东西）。 */
struct TempRoot {
    fs::path path;
    bool ok = false;

    TempRoot() {
        std::error_code ec;
        path = fs::temp_directory_path(ec) / ("lift-plugin-resolve-" + std::to_string(processId()));
        if (ec) return;
        fs::remove_all(path, ec);
        ec.clear();
        fs::create_directories(path, ec);
        ok = !ec;
    }
    ~TempRoot() {
        if (!path.empty()) {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    }
    TempRoot(const TempRoot&) = delete;
    TempRoot& operator=(const TempRoot&) = delete;
};

bool writeFile(const fs::path& path) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) return false;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << "// plugin-resolve 夹具\n";
    return static_cast<bool>(out);
}

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

PluginResolveResult resolve(const std::string& spec, const PluginResolveRoots& roots) {
    return lift::plugin::resolvePluginSpec(spec, roots);
}

}  // namespace

int main() {
    TempRoot tmp;
    LIFT_CHECK(tmp.ok);
    if (!tmp.ok) return lift_test::finish("plugin-resolve");

    // 两个**注入根**：`exe/`（发行布局：`<exeDir>scripts/plugins/`）与 `dev/`（开发根 = 源码 plugins）。
    const fs::path exe_root = tmp.path / "exe";
    const fs::path dev_root = tmp.path / "dev";
    const fs::path exe_plugins = exe_root / "scripts" / "plugins";
    LIFT_CHECK(writeFile(exe_plugins / ".keep"));  // 让目录真的存在
    LIFT_CHECK(writeFile(dev_root / ".keep"));

    PluginResolveRoots roots;
    roots.exe_dir = toUtf8(exe_root);
    roots.dev_dir = toUtf8(dev_root);

    // ---- ① id 语法：机械判据 `^[A-Za-z0-9_-]{1,64}$` ----
    lift_test::g_current_case = "resolve/id-syntax";
    {
        // ★ `-x` / `--` 也**是** id（首字符允许 `-`，用户点名的边界）；别按"像 flag"误判。
        const char* const kYes[] = {"save", "scenario-save07-battle", "A_b-9", "-", "_", "0", "-x", "--"};
        for (const char* yes : kYes) LIFT_CHECK(isPluginId(yes));
        // 64 字符是上界内、65 超界。
        LIFT_CHECK(isPluginId(std::string(64, 'a')));
        LIFT_CHECK(!isPluginId(std::string(65, 'a')));
        const char* const kNo[] = {"", "a.b", "a/b", "a\\b", "a b", "a+b", "säve", "save\n", "C:", "..", "a:b"};
        for (const char* no : kNo) LIFT_CHECK(!isPluginId(no));
        // ★ 65 字符不是 id ⇒ **解析分类**必须落 `Path`（不是"找不到 id"）。
        const std::string long65(65, 'a');
        const PluginResolveResult long_result = resolve(long65, roots);
        LIFT_CHECK(long_result.kind == PluginSpecKind::Path);
        LIFT_CHECK(long_result.path == long65);
        // ★ `-x` / `--` 是 id ⇒ 走 **id 查找**（NotFound：没有 `-x.js`），⛔ 不被当成 flag / 路径。
        for (const char* flagish : {"-x", "--"}) {
            const PluginResolveResult r = resolve(flagish, roots);
            LIFT_CHECK(r.kind == PluginSpecKind::Id);
            LIFT_CHECK(r.status == Status::NotFound);
            LIFT_CHECK(r.searched.size() == 2);  // exe 根 + dev 根都查过（`<id>.js`）
        }
    }

    // ---- ② 路径形式：**原样**（`.js` / `a\b` / 绝对路径 / 空串都走路径，⛔ 不绝对化） ----
    lift_test::g_current_case = "resolve/path-forms";
    {
        const char* const kPaths[] = {"save.js", "a\\b", "a/b", "./x.js", "/abs/x.js", "C:\\x.js", "..", ""};
        for (const char* spec : kPaths) {
            const PluginResolveResult r = resolve(spec, roots);
            LIFT_CHECK(spec[0] == '\0' || isPluginId(spec) == false);  // 前提：这些都不是 id
            LIFT_CHECK(r.status == Status::Ok);
            LIFT_CHECK(r.kind == PluginSpecKind::Path);
            LIFT_CHECK(r.path == spec);  // ⛔ 不绝对化、⛔ 不改写
            LIFT_CHECK(r.searched.empty());
            LIFT_CHECK(r.hits.empty());
        }
        // `save.js` 是路径 ⇒ 即使 dev 根里有 `save.js` 也**不查表**（那正是"换源"的反例）。
        LIFT_CHECK(writeFile(dev_root / "save.js"));
        const PluginResolveResult js = resolve("save.js", roots);
        LIFT_CHECK(js.kind == PluginSpecKind::Path);
        LIFT_CHECK(js.path == "save.js");
        std::error_code ec;
        fs::remove(dev_root / "save.js", ec);
    }

    // ---- ③ id 命中 **exe 根**（发行优先）----
    lift_test::g_current_case = "resolve/id-exe-root";
    {
        LIFT_CHECK(writeFile(exe_plugins / "save.js"));
        PluginResolveRoots only_exe;
        only_exe.exe_dir = roots.exe_dir;  // dev 根空 ⇒ 搜过的候选只有 1 个
        const PluginResolveResult r = resolve("save", only_exe);
        LIFT_CHECK(r.status == Status::Ok);
        LIFT_CHECK(r.kind == PluginSpecKind::Id);
        LIFT_CHECK(r.id == "save");
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(r.hits.size()), 1);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(r.searched.size()), 1);
        if (!r.hits.empty()) LIFT_CHECK(r.path == r.hits.front());
        LIFT_CHECK(fs::path(r.path).is_absolute());  // 命中 ⇒ 绝对路径
        LIFT_CHECK(contains(r.path, "scripts/plugins/save.js") || contains(r.path, "scripts\\plugins\\save.js"));
    }

    // ---- ④ id 命中 **dev 根**（开发回退）----
    lift_test::g_current_case = "resolve/id-dev-root";
    {
        LIFT_CHECK(writeFile(dev_root / "fellow.js"));
        PluginResolveRoots only_dev;
        only_dev.dev_dir = roots.dev_dir;
        const PluginResolveResult r = resolve("fellow", only_dev);
        LIFT_CHECK(r.status == Status::Ok);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(r.searched.size()), 1);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(r.hits.size()), 1);
        LIFT_CHECK(r.path == toUtf8(dev_root / "fellow.js"));
    }

    // ---- ⑤ 两根都命中 ⇒ **Ambiguous**（exit 2 的判据面；⛔ 不静默取第一个）----
    lift_test::g_current_case = "resolve/id-both-roots-ambiguous";
    {
        LIFT_CHECK(writeFile(dev_root / "save.js"));
        const PluginResolveResult r = resolve("save", roots);
        LIFT_CHECK(r.status == Status::Ambiguous);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(r.searched.size()), 2);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(r.hits.size()), 2);
        LIFT_CHECK(r.path.empty());  // 歧义 ⇒ 没有"选中的那一个"
        if (r.hits.size() == 2) {
            // 顺序 = exe 根优先（发行在前、开发在后）
            LIFT_CHECK(r.hits[0] == toUtf8(exe_plugins / "save.js"));
            LIFT_CHECK(r.hits[1] == toUtf8(dev_root / "save.js"));
        }
        std::error_code ec;
        fs::remove(dev_root / "save.js", ec);
    }

    // ---- ⑥ 0 命中 ⇒ **NotFound** 且 `searched` 列出**每个**候选（调用方据此逐行列目录）----
    lift_test::g_current_case = "resolve/id-not-found-lists-candidates";
    {
        const PluginResolveResult r = resolve("ghost", roots);
        LIFT_CHECK(r.status == Status::NotFound);
        LIFT_CHECK(r.path.empty());
        LIFT_CHECK(r.hits.empty());
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(r.searched.size()), 2);
        if (r.searched.size() == 2) {
            LIFT_CHECK(r.searched[0] == toUtf8(exe_plugins / "ghost.js"));
            LIFT_CHECK(r.searched[1] == toUtf8(dev_root / "ghost.js"));
        }
    }

    // ---- ⑦ 空根 ⇒ 不构造假候选（`<空>/scripts/plugins/x.js` 那种噪声不许出现）----
    lift_test::g_current_case = "resolve/empty-roots-skipped";
    {
        PluginResolveRoots none;
        const PluginResolveResult r = resolve("ghost", none);
        LIFT_CHECK(r.status == Status::NotFound);
        LIFT_CHECK(r.searched.empty());
        LIFT_CHECK(r.hits.empty());
    }

    // ---- ⑦' exe 根不可用（`argv[0]` 不含分隔符）⇒ 只剩 dev 根（⛔ 不猜 cwd）----
    lift_test::g_current_case = "resolve/id-empty-exe-root";
    {
        LIFT_CHECK(writeFile(dev_root / "fallback.js"));
        PluginResolveRoots no_exe;
        no_exe.dev_dir = roots.dev_dir;  // exe_dir 空 = 发行根跳过
        const PluginResolveResult r = resolve("fallback", no_exe);
        LIFT_CHECK(r.status == Status::Ok);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(r.searched.size()), 1);
        LIFT_CHECK(r.searched[0] == toUtf8(dev_root / "fallback.js"));
        LIFT_CHECK(r.path == toUtf8(dev_root / "fallback.js"));
    }

    // ---- ⑦'' 两个根指向**同一份文件** ⇒ 候选去重、命中 1 条、**不是歧义** ----
    //   形状：`exe_dir = <tmp>`（候选 `<tmp>/scripts/plugins/dup.js`）、
    //         `dev_dir = <tmp>/scripts/plugins`（候选同一条路径）。
    lift_test::g_current_case = "resolve/duplicate-roots-deduped";
    {
        const fs::path shared_plugins = tmp.path / "scripts" / "plugins";
        LIFT_CHECK(writeFile(shared_plugins / "dup.js"));
        PluginResolveRoots same;
        same.exe_dir = toUtf8(tmp.path);
        same.dev_dir = toUtf8(shared_plugins);
        const PluginResolveResult r = resolve("dup", same);
        LIFT_CHECK(r.status == Status::Ok);  // ⛔ 同一份文件不许报歧义
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(r.searched.size()), 1);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(r.hits.size()), 1);
        LIFT_CHECK(r.path == toUtf8(shared_plugins / "dup.js"));
    }

    // ---- ⑧ 命中必须是**常规文件**（目录不算）----
    lift_test::g_current_case = "resolve/directory-is-not-a-hit";
    {
        std::error_code ec;
        fs::create_directories(dev_root / "adir.js", ec);
        LIFT_CHECK(!ec);
        PluginResolveRoots only_dev;
        only_dev.dev_dir = roots.dev_dir;
        const PluginResolveResult r = resolve("adir", only_dev);
        LIFT_CHECK(r.status == Status::NotFound);  // 目录不是插件文件
        LIFT_CHECK(r.hits.empty());
    }

    // ---- ⑨ 大小写：**按当前卷的实测行为**判（⛔ 不把"非 Windows ⇒ 精确"写死）----
    //   ★ 平台事实：Windows NTFS 默认不敏感；macOS APFS **默认也不敏感**；Linux ext4 敏感。
    //     按 `_WIN32` / `__APPLE__` 猜平台同样会错（APFS 可建成敏感卷、NTFS 可开 per-directory 大小写）
    //     ⇒ 照仓库先例（`tests/file_path_ext.cpp:1204` 的 ⑤）**运行时探测**：在同一目录造两个
    //     只差 ASCII 大小写的文件，数目录项 —— 只有 1 项 = 这个卷不敏感。
    //   ★ 断言的是"解析器跟随当前卷的语义"，不是"哪个平台更对"；日志打一条读数。
    lift_test::g_current_case = "resolve/id-case";
    {
        std::error_code ec;
        const fs::path case_dir = dev_root / "case-probe";
        fs::create_directories(case_dir, ec);
        LIFT_CHECK(!ec);
        LIFT_CHECK(writeFile(case_dir / "CaseProbe.js"));
        LIFT_CHECK(writeFile(case_dir / "caseprobe.js"));  // 不敏感卷上覆盖同一个文件
        std::size_t entries = 0;
        for (fs::directory_iterator it(case_dir, ec), end; it != end; it.increment(ec)) ++entries;
        LIFT_CHECK(!ec);
        const bool case_insensitive = entries < 2;
        std::printf("  [%s] 卷大小写%s（%zu 个目录项）\n", lift_test::g_current_case,
                    case_insensitive ? "不敏感" : "敏感", entries);

        // 被解析的文件必须就在**搜索根**那一层（`<root>/<id>.js`）：探测目录只用来判卷。
        LIFT_CHECK(writeFile(dev_root / "CaseProbe.js"));
        PluginResolveRoots only_dev;
        only_dev.dev_dir = roots.dev_dir;
        const PluginResolveResult lower = resolve("caseprobe", only_dev);
        if (case_insensitive) {
            LIFT_CHECK(lower.status == Status::Ok);  // 不敏感卷：命中大小写不同的文件名
            LIFT_CHECK(lower.path == toUtf8(dev_root / "caseprobe.js"));  // 日志打**构造出的**路径
        } else {
            LIFT_CHECK(lower.status == Status::NotFound);  // 敏感卷：精确匹配
        }
        const PluginResolveResult exact = resolve("CaseProbe", only_dev);
        LIFT_CHECK(exact.status == Status::Ok);
        LIFT_CHECK(exact.path == toUtf8(dev_root / "CaseProbe.js"));
    }

    // =====================================================================================
    // S2 · 保留 id `hud`（`--overlay` 预设 = `--plugin hud`，两个入口同一解析）
    //   ① 保留根 = `<exe>/scripts/ui/hud.js` → `LIFT_UI_SCRIPT_DIR/hud.js`（优先序，exe 赢）；
    //   ② 用户插件根里出现 `hud.js` ⇒ **ReservedShadowed**（⛔ 不静默被内置件遮住）；
    //   ③ 保留 id **不**从普通根命中（找不到时列出的是保留根，不是 scripts/plugins）。
    // =====================================================================================
    const fs::path exe_ui = exe_root / "scripts" / "ui";
    const fs::path hud_dev = tmp.path / "ui-dev";
    PluginResolveRoots hud_roots = roots;
    hud_roots.hud_dev_dir = toUtf8(hud_dev);

    // ---- ⑩ 保留根：只有 dev ⇒ 命中 dev 的 hud.js ----
    lift_test::g_current_case = "resolve/hud-dev-root";
    {
        LIFT_CHECK(writeFile(hud_dev / "hud.js"));
        const PluginResolveResult r = resolve("hud", hud_roots);
        LIFT_CHECK(r.status == Status::Ok);
        LIFT_CHECK(r.kind == PluginSpecKind::Id);
        LIFT_CHECK(r.id == "hud");
        LIFT_CHECK(r.path == toUtf8(hud_dev / "hud.js"));
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(r.hits.size()), 1);
    }

    // ---- ⑪ 保留根：exe 与 dev 都在 ⇒ **exe 赢**（优先序，⛔ 不报歧义 —— 与 S2 之前同一条）----
    lift_test::g_current_case = "resolve/hud-exe-wins";
    {
        LIFT_CHECK(writeFile(exe_ui / "hud.js"));
        const PluginResolveResult r = resolve("hud", hud_roots);
        LIFT_CHECK(r.status == Status::Ok);
        LIFT_CHECK(r.path == toUtf8(exe_ui / "hud.js"));
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(r.searched.size()), 2);  // 两个保留根都试过
    }

    // ---- ⑫ 用户插件根里有 hud.js ⇒ ReservedShadowed（列**用户那份**的路径，exit 2 的判据面）----
    lift_test::g_current_case = "resolve/hud-shadowed-exe-root";
    {
        LIFT_CHECK(writeFile(exe_plugins / "hud.js"));
        const PluginResolveResult r = resolve("hud", hud_roots);
        LIFT_CHECK(r.status == Status::ReservedShadowed);
        LIFT_CHECK(r.path.empty());
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(r.hits.size()), 1);
        if (!r.hits.empty()) LIFT_CHECK(r.hits[0] == toUtf8(exe_plugins / "hud.js"));
        std::error_code ec;
        fs::remove(exe_plugins / "hud.js", ec);
    }
    lift_test::g_current_case = "resolve/hud-shadowed-dev-root";
    {
        LIFT_CHECK(writeFile(dev_root / "hud.js"));
        const PluginResolveResult r = resolve("hud", hud_roots);
        LIFT_CHECK(r.status == Status::ReservedShadowed);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(r.hits.size()), 1);
        if (!r.hits.empty()) LIFT_CHECK(r.hits[0] == toUtf8(dev_root / "hud.js"));
        std::error_code ec;
        fs::remove(dev_root / "hud.js", ec);
    }

    // ---- ⑬ 保留根里都没了 ⇒ NotFound 列**保留候选**（⛔ 不是 scripts/plugins 的）----
    lift_test::g_current_case = "resolve/hud-not-found-lists-reserved-roots";
    {
        std::error_code ec;
        fs::remove(exe_ui / "hud.js", ec);
        ec.clear();
        fs::remove(hud_dev / "hud.js", ec);
        const PluginResolveResult r = resolve("hud", hud_roots);
        LIFT_CHECK(r.status == Status::NotFound);
        LIFT_CHECK(r.path.empty());
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(r.searched.size()), 2);
        if (r.searched.size() == 2) {
            LIFT_CHECK(r.searched[0] == toUtf8(exe_ui / "hud.js"));
            LIFT_CHECK(r.searched[1] == toUtf8(hud_dev / "hud.js"));
            LIFT_CHECK(r.searched[0].find("scripts/plugins") == std::string::npos);
        }
    }

    // ---- ⑭ 路径形式 `hud.js` **不**享受保留语义（仍是原样的路径）----
    lift_test::g_current_case = "resolve/hud-path-form-not-reserved";
    {
        const PluginResolveResult r = resolve("hud.js", hud_roots);
        LIFT_CHECK(r.status == Status::Ok);
        LIFT_CHECK(r.kind == PluginSpecKind::Path);
        LIFT_CHECK(r.path == "hud.js");
        LIFT_CHECK(r.searched.empty());
    }

    return lift_test::finish("plugin-resolve");
}
