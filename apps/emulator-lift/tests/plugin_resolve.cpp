// emulator-lift · `--plugin <id|path>` **纯解析器**的判据（`plugin-resolve`，S1）
//
// 红了意味着什么（三条都是"改了就是 bug、而且**没有运行期症状**"）：
//   ① id 语法被改松 / 改紧：`.js` 后缀被当成 id ⇒ 今天的 `--plugin x.js` 从"cwd 相对文件"
//      静默变成"白名单根里的同名文件"（**换源**）；`..` 被放进来 ⇒ 搜索面偷偷扩大。
//   ② 搜索根被加宽 / 改顺序：把 `lib/` 也搜、递归子目录、或"第一个命中赢" ⇒
//      用户装了另一份同名文件就会被装上（或歧义被静默吃掉）。
//   ③ 路径形式被绝对化 / 早检查：cwd 相对 / 绝对 / "装载失败 exit 2" 三条行为都会变。
//
// ★ 判据只碰 `temp_directory_path()` 下的**注入 roots**（临时目录 + 几个 `.js` 文件）：
//   零 exe / 零 GPU / 零资产 / 零游戏安装 ⇒ 顶层无条件注册 = pure 档（`AGENTS.md` §12.4）。
// ★ 断言的是**分类 / 顺序 / 候选清单**，⛔ 不断言任何游戏读数、⛔ 不锚具体路径字符串（除临时根）。
// ★ 大小写那条是**平台分支显式写出来**的（Windows 文件系统不敏感 / 其余精确）：两个平台都能跑，
//   但谁也不会把"另一个平台的语义"当本平台的结论。
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
        const char* const kYes[] = {"save", "scenario-save07-battle", "A_b-9", "-", "_", "0"};
        for (const char* yes : kYes) LIFT_CHECK(isPluginId(yes));
        // 64 字符是上界内、65 超界。
        LIFT_CHECK(isPluginId(std::string(64, 'a')));
        LIFT_CHECK(!isPluginId(std::string(65, 'a')));
        const char* const kNo[] = {"", "a.b", "a/b", "a\\b", "a b", "a+b", "säve", "save\n", "C:", "..", "a:b"};
        for (const char* no : kNo) LIFT_CHECK(!isPluginId(no));
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

    // ---- ⑨ 大小写：Windows 不敏感 / 其余精确（**平台分支显式写出来**）----
    //   ★ 不断言"哪个平台更好"：Windows 的 `CreateFileW` 就是不敏感，仓库又有"零大小写冲突"纪律
    //     ⇒ 这里只钉"当前平台的语义没有被悄悄改成另一套"。
    lift_test::g_current_case = "resolve/id-case";
    {
        LIFT_CHECK(writeFile(dev_root / "CaseProbe.js"));
        PluginResolveRoots only_dev;
        only_dev.dev_dir = roots.dev_dir;
        const PluginResolveResult lower = resolve("caseprobe", only_dev);
#ifdef _WIN32
        LIFT_CHECK(lower.status == Status::Ok);  // Windows：命中大小写不同的文件名
        LIFT_CHECK(lower.path == toUtf8(dev_root / "caseprobe.js"));  // 日志打**构造出的**路径
#else
        LIFT_CHECK(lower.status == Status::NotFound);  // POSIX：精确匹配
#endif
        const PluginResolveResult exact = resolve("CaseProbe", only_dev);
        LIFT_CHECK(exact.status == Status::Ok);
        LIFT_CHECK(exact.path == toUtf8(dev_root / "CaseProbe.js"));
    }

    return lift_test::finish("plugin-resolve");
}
