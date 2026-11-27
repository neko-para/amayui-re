// emulator-lift · C2a 的 **headless e2e**：`--present headless --plugin <file>` 真的能装载 + 跑一条 Run
//
// 判据（红了意味着什么）：
//   ① 子进程 **exit 0**（`--time-limit 60` 属正常结局：`TimeLimit` 在 `aotStopOk()` 里）；
//   ② stdout 有**一次性** `plugin    : ` 行（`file=` / `entry=default` / `callbacks=yes` / `run=done`）；
//      红了 ⇒ 读点观察者没接上（`start` 不跑 / `atReadPoint` 不触发 / Run 收不了口）。
//   ③ stderr 里**没有** `render is defined but no window`（本夹具没有 render）——它只该在
//      `--plugin` 带 render + headless 时出现一次（D-C2-5）；
//   ④ `plugin    : ` **只出现一次**（⛔ 不是每帧 / 每读点一行；AGENTS §12.8 第 3 条）。
//   ⑤ `plugin/quit-attribution`：`ctx.quit()` ⇒ readout `run=quit` + 停机摘要点名 `plugin: quit`。
//   ⑥ `plugin/bad-config-exit2`：坏 `plugin-config.json` ⇒ 产品 exe **exit 2** + stderr 点名
//      `plugin-config.json`（配置装载失败是**启动失败**，不是 warn + 继续）。
//
// ★ 缺执行源 / 安装树 / GPU / 宿主字体 ⇒ `LIFT_SKIP(77)`（⛔ 不假绿；输入假定在场）。
// ★ 子进程统一走 `tests/support/subprocess.h` 的 `runChild`（跨平台；capture = 两个临时文件）。
// ★ `--appdata` 指到临时目录 ⇒ 不往仓库 / 真 AppData 写东西。
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "check.h"
#include "support/subprocess.h"

namespace {

namespace fs = std::filesystem;

std::size_t countOccurrences(const std::string& text, const std::string& needle) {
    std::size_t n = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) {
        ++n;
    }
    return n;
}

// 写一份 UTF-8 文本（父目录必须已存在；只给 `plugin-config.json` 用）。
bool writeTextFile(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.flush();
    return static_cast<bool>(out);
}

}  // namespace

int main(int argc, char** argv) {
    using namespace lift_test;

    if (argc < 4) {
        LIFT_SKIP("需要 CTest 传 <emulator-lift-aot> <AGE.EXE__dumped.sectfix.EXE> <安装树>（见 CMakeLists）");
    }
    const fs::path exe = fs::path(argv[1]);
    const fs::path age_exe = fs::path(argv[2]);
    const fs::path game_dir = fs::path(argv[3]);
    const fs::path fixture = fs::path(LIFT_PLUGIN_FIXTURES_DIR) / "headless.js";
    if (!fs::exists(exe)) LIFT_SKIP("emulator-lift-aot 不在场");
    if (!fs::exists(age_exe)) LIFT_SKIP("执行源（corpus/assets/age/AGE.EXE__dumped.sectfix.EXE）不在场");
    if (!fs::exists(game_dir)) LIFT_SKIP("安装树（--game-dir）不在场");
    if (!fs::exists(fixture)) LIFT_SKIP("夹具 tests/plugin_fixtures/headless.js 不在场");


    std::error_code ec;
    const fs::path root = fs::temp_directory_path(ec) /
                          ("lift-plugin-headless-" + std::to_string(lift_test::processId()));
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    LIFT_CHECK(!ec);
    const fs::path appdata = root / "appdata";
    fs::create_directories(appdata, ec);

    g_current_case = "plugin/headless";
    const std::vector<std::string> args = {
        "--exe",
        pathToUtf8(age_exe),
        "--game-dir",
        pathToUtf8(game_dir),
        "--appdata",
        pathToUtf8(appdata),
        // ★ 缺省档 = headless；这里显式给一遍（判据就是"没有窗口也能跑 Run"）。
        "--present",
        "headless",
        "--skip-logo",
        "--audio",
        "off",
        "--time-limit",
        "60",
        "--plugin",
        pathToUtf8(fixture),
    };
    ChildOptions opts;
    opts.cwd = root;
    opts.timeout = std::chrono::milliseconds(120000);
    const ChildResult r = runChild(exe, args, opts);
    LIFT_CHECK(r.started);
    if (!r.started) return lift_test::finish("plugin-headless");
    std::printf("plugin-headless: exit=%d timed_out=%d\n", r.exit_code, r.timed_out ? 1 : 0);

    // ---- 缺资产 / GPU / 字体 ⇒ skip（⛔ 不假红：这些是"输入不在场"，不是"接线坏了"）----
    if (r.timed_out) {
        LIFT_CHECK(false);
        std::fprintf(stderr, "  stderr:\n%s\n", r.err.c_str());
        return lift_test::finish("plugin-headless");
    }
    if (r.err.find("宿主字体不可用") != std::string::npos) LIFT_SKIP("缺宿主字体（LIFT_HOST_FONT_PATH）");
    if (r.err.find("没有生成物") != std::string::npos) LIFT_SKIP("AOT 生成物不在场（缺 clang / 执行源）");
    // ★ headless **也要** GPU device（隐藏窗口 + 光栅后端）⇒ 没有 GPU 时 `aot_main` 拒绝启动
    //   （`拒绝启动 —— …`）。那是"输入不在场"，不是"接线坏了"（⛔ 不假红）。
    if (r.err.find("拒绝启动") != std::string::npos) LIFT_SKIP("GPU / 窗口初始化不成立（headless 也要 GPU device）");

    // ---- 判据 ①：exit 0（`--time-limit` 属正常结局）----
    if (r.exit_code != 0) {
        std::fprintf(stderr, "  stdout:\n%s\n  stderr:\n%s\n", r.out.c_str(), r.err.c_str());
    }
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(r.exit_code), 0u);

    // ---- 判据 ②：一次性 readout 行 + Run 收口 ----
    LIFT_CHECK(r.out.find("plugin    : ") != std::string::npos);
    LIFT_CHECK(r.out.find("entry=default") != std::string::npos);
    LIFT_CHECK(r.out.find("callbacks=yes") != std::string::npos);
    LIFT_CHECK(r.out.find("run=done") != std::string::npos);
    // ---- 判据 ③：本夹具没有 render ⇒ 那条一次性说明**不该**出现 ----
    LIFT_CHECK(r.err.find("render is defined but no window") == std::string::npos);
    // ---- 判据 ④：`plugin    : ` 只出现一次（⛔ 不周期）----
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(countOccurrences(r.out, "plugin    : ")), 1);
    if (r.out.find("plugin    : ") == std::string::npos) {
        std::fprintf(stderr, "  stdout:\n%s\n  stderr:\n%s\n", r.out.c_str(), r.err.c_str());
    }

    // ---- ⑤ `ctx.quit()`：第二条停机源 + 归因 `plugin: quit`（D-C2-9）----
    //   判据：exit 0（`Stopped` 属正常结局）、readout `run=quit`、摘要的停机行点名 `plugin: quit`
    //   （⛔ 不是旧的 `input-plan: quit` —— 那正是"归因没接上"的症状）。
    g_current_case = "plugin/quit-attribution";
    {
        const fs::path quit_appdata = root / "appdata-quit";
        fs::create_directories(quit_appdata, ec);
        const std::vector<std::string> quit_args = {
            "--exe",
            pathToUtf8(age_exe),
            "--game-dir",
            pathToUtf8(game_dir),
            "--appdata",
            pathToUtf8(quit_appdata),
            "--present",
            "headless",
            "--skip-logo",
            "--audio",
            "off",
            "--time-limit",
            "60",
            "--plugin",
            pathToUtf8(fs::path(LIFT_PLUGIN_FIXTURES_DIR) / "quit.js"),
        };
        ChildOptions quit_opts;
        quit_opts.cwd = root;
        quit_opts.timeout = std::chrono::milliseconds(120000);
        const ChildResult q = runChild(exe, quit_args, quit_opts);
        LIFT_CHECK(q.started);
        LIFT_CHECK(!q.timed_out);
        if (q.exit_code != 0) {
            std::fprintf(stderr, "  stdout:\n%s\n  stderr:\n%s\n", q.out.c_str(), q.err.c_str());
        }
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(q.exit_code), 0u);
        LIFT_CHECK(q.out.find("plugin    : ") != std::string::npos);
        LIFT_CHECK(q.out.find("run=quit") != std::string::npos);
        LIFT_CHECK(q.out.find("plugin: quit") != std::string::npos);
        LIFT_CHECK(q.out.find("input-plan: quit") == std::string::npos);
    }

    // ---- ⑥ `plugin/bad-config-exit2`：坏 plugin-config.json ⇒ **进程 exit 2** ----
    //   判据（红了意味着什么）：配置装载失败被降级成 warn + 继续（D1 41 的"exit 2"没落地）——
    //   插件会带着"看起来没有配置"的状态继续跑，坏文件永远不会被人发现。
    //   ★ 直接写坏 JSON（不是形状错），文件路径 = 本用例的私有 appdata 根。
    g_current_case = "plugin/bad-config-exit2";
    {
        const fs::path bad_appdata = root / "appdata-bad-config";
        fs::create_directories(bad_appdata, ec);
        LIFT_CHECK(writeTextFile(bad_appdata / "plugin-config.json", R"({"version":1,"domains":{)"));
        const std::vector<std::string> bad_args = {
            "--exe",
            pathToUtf8(age_exe),
            "--game-dir",
            pathToUtf8(game_dir),
            "--appdata",
            pathToUtf8(bad_appdata),
            "--present",
            "headless",
            "--skip-logo",
            "--audio",
            "off",
            "--time-limit",
            "30",
            "--plugin",
            pathToUtf8(fixture),
        };
        ChildOptions bad_opts;
        bad_opts.cwd = root;
        bad_opts.timeout = std::chrono::milliseconds(60000);
        const ChildResult bad = runChild(exe, bad_args, bad_opts);
        LIFT_CHECK(bad.started);
        LIFT_CHECK(!bad.timed_out);
        if (bad.exit_code != 2 || bad.err.find("配置装载失败（exit 2）") == std::string::npos ||
            bad.err.find("plugin-config.json") == std::string::npos) {
            std::fprintf(stderr, "  stdout:\n%s\n  stderr:\n%s\n", bad.out.c_str(), bad.err.c_str());
        }
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(bad.exit_code), 2);
        LIFT_CHECK(bad.err.find("配置装载失败（exit 2）") != std::string::npos);
        LIFT_CHECK(bad.err.find("plugin-config.json") != std::string::npos);
        // ⛔ 坏配置不许被当成"没有配置"继续跑（没有 readout = 没进 Run）。
        LIFT_CHECK(bad.out.find("plugin    : ") == std::string::npos);
    }
    return lift_test::finish("plugin-headless");
}
