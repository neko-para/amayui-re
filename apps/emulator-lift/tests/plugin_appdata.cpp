// emulator-lift · **`ctx.appdata.stat`** 的 pure 判据（`plugin-appdata`）
//
// 判据（红了意味着什么）——`ctx.appdata` 是插件能碰到的**唯一文件系统面**：
//   · `appdata/stat-file`    ：普通文件 ⇒ exists + isFile + size（bytes）。
//   · `appdata/stat-dir`     ：目录 / 归一化的相对路径（`sub/./dir`、`sub\dir`）⇒ exists + isDir。
//   · `appdata/stat-missing` ：不存在 ⇒ exists=false + `not_found`（⛔ 不是异常）；中间段是文件 ⇒ `not_dir`。
//   · `appdata/stat-escape`  ：`..` 段 ⇒ `invalid_path`（先拒绝，不做"归一化后再判"）。
//   · `appdata/stat-absolute`：宿主绝对路径 / UNC ⇒ `invalid_path`（⛔ 不 clamp 到 appdata 根）。
//   · `appdata/stat-empty`   ：空串 / 只有 `.` / 分隔符 ⇒ `invalid_path`。
//   · `appdata/stat-nul`     ：内嵌 NUL / 控制字符 ⇒ `invalid_path`（`std::filesystem` 会截断）。
//   · `appdata/stat-no-appdata`：没 attach ⇒ available=false + path 空 + 一律 `no_appdata`。
//   · `appdata/stat-symlink-escape`：根下的符号链接指向外面 ⇒ `escape`（canonical 后不在根下）。
//     ★ Windows 建符号链接需要权限；建不了时**打印 SKIP 原因并只跳过这一条**（其它断言照跑）。
//   · `appdata/attach-rejects-install-tree`：`--appdata` 落在 `--game-dir` 里 ⇒ 不接（available=false）。
//
// ★ 端口：本用例只碰 `std::filesystem::temp_directory_path()` 下的临时目录（零游戏安装 / 零 GPU /
//   零真窗口）⇒ 顶层无条件注册 = pure 档（`AGENTS.md` §12.4）。
// ★ 判据只认**稳定错误码**（`AppDataStatError`），⛔ 不断言平台 errno / 文案。
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include "check.h"
#include "plugin/plugin_appdata.h"
#include "runtime/host_encoding.h"

namespace {

namespace fs = std::filesystem;
using lift::plugin::AppDataStat;
using lift::plugin::AppDataStatError;
using lift::plugin::PluginAppData;

std::uint32_t processId() {
#ifdef _WIN32
    return static_cast<std::uint32_t>(::_getpid());
#else
    return static_cast<std::uint32_t>(::getpid());
#endif
}

std::string toUtf8(const fs::path& p) {
#ifdef _WIN32
    return lift::wideToUtf8(p.wstring().c_str());
#else
    return p.string();
#endif
}

/** 临时树：`<TEMP>/lift-plugin-appdata-<pid>`；析构删掉（里面只有本用例造的文件）。 */
struct TempRoot {
    fs::path path;
    bool ok = false;

    TempRoot() {
        std::error_code ec;
        path = fs::temp_directory_path(ec) / ("lift-plugin-appdata-" + std::to_string(processId()));
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

bool writeFile(const fs::path& path, const std::string& bytes) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

bool expectError(const char* what, const AppDataStat& st, AppDataStatError want) {
    LIFT_CHECK(!st.exists);
    if (st.error != want) {
        lift_test::failf(__FILE__, __LINE__, "%s: 期望错误码 %s，实际 %s", what,
                         lift::plugin::appDataStatErrorName(want),
                         lift::plugin::appDataStatErrorName(st.error));
        return false;
    }
    return true;
}

}  // namespace

int main() {
    TempRoot temp;
    if (!temp.ok) {
        LIFT_SKIP("取不到宿主临时目录 / 建不了临时树");
    }
    const fs::path root = temp.path / "appdata";
    std::error_code ec;
    fs::create_directories(root / "sub" / "dir", ec);
    LIFT_CHECK(!ec);
    if (ec) return lift_test::finish("plugin-appdata");
    LIFT_CHECK(writeFile(root / "file.txt", "hello"));

    // ---- appdata/stat-no-appdata：没 attach ⇒ 不能用 ----
    lift_test::g_current_case = "appdata/stat-no-appdata";
    {
        PluginAppData none;
        LIFT_CHECK(!none.available());
        LIFT_CHECK(none.path().empty());
        expectError("stat-no-appdata", none.stat("file.txt"), AppDataStatError::NoAppData);
        // 空串 = "没给 --appdata"（正常；不是错误）⇒ available=false。
        std::string err;
        PluginAppData unset;
        LIFT_CHECK(unset.attach("", "", &err));
        LIFT_CHECK(err.empty());
        LIFT_CHECK(!unset.available());
    }

    // ---- attach：绝对化 + available ----
    lift_test::g_current_case = "appdata/attach-absolute-path";
    PluginAppData appdata;
    {
        std::string err;
        LIFT_CHECK(appdata.attach(toUtf8(root), "", &err));
        LIFT_CHECK(err.empty());
        LIFT_CHECK(appdata.available());
        const fs::path expected = fs::absolute(root).lexically_normal();
        LIFT_CHECK(appdata.path() == toUtf8(expected));
        // 相对路径也接受（`fs::absolute` 以宿主 cwd 为基准）——只判"可绝对化"，不判具体 cwd。
        PluginAppData relative;
        std::string rel_err;
        LIFT_CHECK(relative.attach(".", "", &rel_err));
        LIFT_CHECK(relative.available());
    }

    // ---- appdata/stat-file：普通文件 ----
    lift_test::g_current_case = "appdata/stat-file";
    {
        const AppDataStat st = appdata.stat("file.txt");
        LIFT_CHECK(st.exists);
        LIFT_CHECK(st.is_file);
        LIFT_CHECK(!st.is_dir);
        LIFT_CHECK(st.error == AppDataStatError::None);
        LIFT_CHECK(st.has_size);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(st.size), 5);
    }

    // ---- appdata/stat-dir：目录 / 分隔符与 `.` 归一化 ----
    lift_test::g_current_case = "appdata/stat-dir";
    {
        const AppDataStat sub = appdata.stat("sub");
        LIFT_CHECK(sub.exists && sub.is_dir && !sub.is_file);
        const AppDataStat nested = appdata.stat("sub/dir");
        LIFT_CHECK(nested.exists && nested.is_dir);
        const AppDataStat dots = appdata.stat("sub/./dir");
        LIFT_CHECK(dots.exists && dots.is_dir);
        const AppDataStat backslash = appdata.stat("sub\\dir");
        LIFT_CHECK(backslash.exists && backslash.is_dir);
        // 根自身（`.`）不是"相对路径里的合法目标"⇒ 归一化后为空 ⇒ invalid_path。
        expectError("stat-dot", appdata.stat("."), AppDataStatError::InvalidPath);
    }

    // ---- appdata/stat-missing：不存在 / 中间段不是目录 ----
    lift_test::g_current_case = "appdata/stat-missing";
    {
        expectError("stat-missing", appdata.stat("nope.txt"), AppDataStatError::NotFound);
        expectError("stat-missing-nested", appdata.stat("sub/nope.txt"), AppDataStatError::NotFound);
        const AppDataStat not_dir = appdata.stat("file.txt/child");
        LIFT_CHECK(!not_dir.exists);
        LIFT_CHECK(not_dir.error == AppDataStatError::NotDir);
    }

    // ---- appdata/stat-escape：`..` 一律先拒绝 ----
    lift_test::g_current_case = "appdata/stat-escape";
    {
        expectError("escape-parent", appdata.stat("../x"), AppDataStatError::InvalidPath);
        expectError("escape-middle", appdata.stat("sub/../x"), AppDataStatError::InvalidPath);
        expectError("escape-deep", appdata.stat("sub/../../x"), AppDataStatError::InvalidPath);
    }

    // ---- appdata/stat-absolute：宿主绝对路径 / UNC ----
    lift_test::g_current_case = "appdata/stat-absolute";
    {
        expectError("absolute-host-path", appdata.stat(toUtf8(root / "file.txt")),
                    AppDataStatError::InvalidPath);
        // 前导分隔符（root-relative）也不是"相对 appdata 根"。
        expectError("absolute-leading-slash", appdata.stat("/file.txt"), AppDataStatError::InvalidPath);
        expectError("absolute-leading-backslash", appdata.stat("\\file.txt"),
                    AppDataStatError::InvalidPath);
#ifdef _WIN32
        expectError("absolute-unc", appdata.stat("\\\\server\\share\\file.txt"),
                    AppDataStatError::InvalidPath);
        expectError("absolute-drive-relative", appdata.stat("C:file.txt"),
                    AppDataStatError::InvalidPath);
#endif
    }

    // ---- appdata/stat-empty：空串 / 只有分隔符 ----
    lift_test::g_current_case = "appdata/stat-empty";
    {
        expectError("empty", appdata.stat(""), AppDataStatError::InvalidPath);
        expectError("empty-slashes", appdata.stat("///"), AppDataStatError::InvalidPath);
        expectError("empty-dot", appdata.stat("./"), AppDataStatError::InvalidPath);
    }

    // ---- appdata/stat-nul：NUL / 控制字符 ----
    lift_test::g_current_case = "appdata/stat-nul";
    {
        expectError("nul", appdata.stat(std::string("a\0b", 3)), AppDataStatError::InvalidPath);
        expectError("control", appdata.stat(std::string("a\nb", 3)), AppDataStatError::InvalidPath);
    }

    // ---- appdata/stat-invalid-utf8：非法 UTF-8 不许退化成"根目录" ----
    lift_test::g_current_case = "appdata/stat-invalid-utf8";
    {
        // U+D800 的 WTF-8（QJS 对孤立代理项的编码）；utf8ToWide 会返回空串 ⇒ 不拒绝就会 stat 根。
        expectError("utf8-surrogate", appdata.stat(std::string("\xED\xA0\x80", 3)),
                    AppDataStatError::InvalidPath);
        expectError("utf8-truncated", appdata.stat(std::string("\xC3", 1)),
                    AppDataStatError::InvalidPath);
        expectError("utf8-in-segment", appdata.stat(std::string("sub/\xED\xA0\x80", 7)),
                    AppDataStatError::InvalidPath);
    }

    // ---- appdata/attach-rejects-install-tree：appdata 落在只读安装树里 ⇒ 不接 ----
    lift_test::g_current_case = "appdata/attach-rejects-install-tree";
    {
        PluginAppData nested;
        std::string err;
        LIFT_CHECK(!nested.attach(toUtf8(root / "sub"), toUtf8(root), &err));
        LIFT_CHECK(!err.empty());
        LIFT_CHECK(!nested.available());
        expectError("install-tree", nested.stat("dir"), AppDataStatError::NoAppData);
    }

    // ---- appdata/attach-not-a-directory：--appdata 指向普通文件 ⇒ 不接（warn + 继续，⛔ 不是 exit 2）----
    lift_test::g_current_case = "appdata/attach-not-a-directory";
    {
        PluginAppData file_root;
        std::string err;
        LIFT_CHECK(!file_root.attach(toUtf8(root / "file.txt"), "", &err));
        LIFT_CHECK(!err.empty());
        LIFT_CHECK(!file_root.available());
    }

    // ---- appdata/stat-symlink-escape：符号链接 / junction 指向根外 ----
    lift_test::g_current_case = "appdata/stat-symlink-escape";
    {
        const fs::path outside = temp.path / "outside";
        LIFT_CHECK(writeFile(outside / "evil.txt", "outside"));
        std::error_code link_ec;
        fs::create_directory_symlink(outside, root / "escape-link", link_ec);
        if (link_ec) {
            // ⛔ 不 printf 后继续（那会被记成绿）：环境不成立 ⇒ skip（CMake SKIP_RETURN_CODE 77）。
            LIFT_SKIP("建不了符号链接（需开发者模式 / 权限）⇒ escape 判据不可判定");
        } else {
            expectError("symlink-dir", appdata.stat("escape-link"), AppDataStatError::Escape);
            expectError("symlink-child", appdata.stat("escape-link/evil.txt"), AppDataStatError::Escape);
            // 反向自检：链接目标确实存在（否则"escape"可能只是 not_found 的另一种说法）。
            LIFT_CHECK(fs::exists(outside / "evil.txt"));
        }
    }

    return lift_test::finish("plugin-appdata");
}
