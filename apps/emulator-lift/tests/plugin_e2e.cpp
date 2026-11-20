// emulator-lift · C2a 的 **headless e2e**：`--present headless --plugin <file>` 真的能装载 + 跑一条 Run
//
// 判据（红了意味着什么）：
//   ① 子进程 **exit 0**（`--time-limit 60` 属正常结局：`TimeLimit` 在 `aotStopOk()` 里）；
//   ② stdout 有**一次性** `plugin    : ` 行（`file=` / `entry=default` / `callbacks=yes` / `run=done`）；
//      红了 ⇒ 读点观察者没接上（`start` 不跑 / `atReadPoint` 不触发 / Run 收不了口）。
//   ③ stderr 里**没有** `render is defined but no window`（本夹具没有 render）——它只该在
//      `--plugin` 带 render + headless 时出现一次（D-C2-5）；
//   ④ `plugin    : ` **只出现一次**（⛔ 不是每帧 / 每读点一行；AGENTS §12.8 第 3 条）。
//   ⑤ `plugin/load-save-e2e`（C2c）：真库 `scripts/plugins/lib/save.js` + `--plugin-param slot=5`
//      走完整条链；判据 = readout `run=done` / `params=slot=5` / `calls ∈ {6,7}`（D2c-2：第二道
//      确认也是合法终态）+ stderr 终态日志含 **FIELD** + 无 `run failed`（⛔ 不看 exit code）。
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
    // ★ C2c：`plugin/load-save-e2e` 直接拿**真库**当入口（import 根 = 入口文件目录 ⇒ 不复制文件）。
    const fs::path save_js = fs::path(LIFT_PLUGIN_LIB_DIR) / "save.js";
    const fs::path saves_dir = fs::path(LIFT_SAVES_DIR);
    if (!fs::exists(exe)) LIFT_SKIP("emulator-lift-aot 不在场");
    if (!fs::exists(age_exe)) LIFT_SKIP("执行源（corpus/assets/age/AGE.EXE__dumped.sectfix.EXE）不在场");
    if (!fs::exists(game_dir)) LIFT_SKIP("安装树（--game-dir）不在场");
    if (!fs::exists(fixture)) LIFT_SKIP("夹具 tests/plugin_fixtures/headless.js 不在场");
    if (!fs::exists(save_js)) LIFT_SKIP("真库 apps/emulator-lift/scripts/plugins/lib/save.js 不在场");
    if (!fs::exists(saves_dir / "SAVE05.DAT")) LIFT_SKIP("saves/SAVE05.DAT 不在场（saves/ 只读复现素材）");

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

    // ---- ⑥ C2c `plugin/load-save-e2e`：真库 `lib/save.js` + `--plugin-param slot=5` 的整条链 ----
    //   判据 = **stdout 的一次性 readout**（⛔ 不是 exit code：`ctx.finish` 不改进程 exit，且
    //   `--time-limit 90` 是正常结局）：`run=done` + `params=slot=5` + `calls ∈ {6,7}`；
    //   stderr 的终态日志（`ctx.log`）必须点名 **FIELD**，且**没有** `run failed`；readout 只一行。
    //   红了 ⇒ 某道门没命中 / 坐标错（hang 成 `run=failed reason=…gate timeout`）—— 纯档的
    //   `plugin-runtime` 已经钉住链形状，这里钉"真 guest 里这条链真的走到 FIELD"。
    g_current_case = "plugin/load-save-e2e";
    {
        // `.tmp/` 私有 appdata：`saves/` **只读**，照 `.tmp/save05-bench/clean-appdata.sh` 复制。
        const fs::path repo_tmp = saves_dir.parent_path() / ".tmp";
        const fs::path save_root =
            repo_tmp / ("lift-plugin-load-save-" + std::to_string(lift_test::processId()));
        fs::remove_all(save_root, ec);
        const fs::path load_appdata = save_root / "appdata";
        const fs::path guest_save_dir = load_appdata / "Local" / "Eushully" /
                                        fs::path(std::u8string(u8"天結いキャッスルマイスター")) / "SAVE";
        fs::create_directories(guest_save_dir, ec);
        LIFT_CHECK(!ec);
        std::size_t copied = 0;
        std::error_code copy_ec;
        for (fs::directory_iterator it(saves_dir, copy_ec); !copy_ec && it != fs::directory_iterator();
             it.increment(copy_ec)) {
            std::error_code entry_ec;
            if (!it->is_regular_file(entry_ec) || entry_ec) continue;
            const std::string ext = it->path().extension().string();
            if (ext != ".DAT" && ext != ".STH") continue;
            fs::copy_file(it->path(), guest_save_dir / it->path().filename(),
                          fs::copy_options::overwrite_existing, copy_ec);
            if (!copy_ec) ++copied;
        }
        LIFT_CHECK(!copy_ec);
        LIFT_CHECK(copied > 0);

        const std::vector<std::string> load_args = {
            "--exe",
            pathToUtf8(age_exe),
            "--game-dir",
            pathToUtf8(game_dir),
            "--appdata",
            pathToUtf8(load_appdata),
            "--present",
            "headless",
            "--skip-logo",
            "--audio",
            "off",
            "--time-limit",
            "90",
            "--plugin",
            pathToUtf8(save_js),
            "--plugin-param",
            "slot=5",
        };
        ChildOptions ls_opts;
        ls_opts.cwd = save_root;
        ls_opts.timeout = std::chrono::milliseconds(120000);
        const ChildResult ls = runChild(exe, load_args, ls_opts);
        LIFT_CHECK(ls.started);
        std::printf("plugin/load-save-e2e: exit=%d timed_out=%d\n", ls.exit_code, ls.timed_out ? 1 : 0);
        if (!ls.started) return lift_test::finish("plugin-headless");
        if (ls.timed_out) {
            LIFT_CHECK(false);
            std::fprintf(stderr, "  stdout:\n%s\n  stderr:\n%s\n", ls.out.c_str(), ls.err.c_str());
            return lift_test::finish("plugin-headless");
        }
        // ---- 缺资产 / GPU / 字体 ⇒ skip（与主用例同一条口径；⛔ 不假红）----
        if (ls.err.find("宿主字体不可用") != std::string::npos) LIFT_SKIP("缺宿主字体（LIFT_HOST_FONT_PATH）");
        if (ls.err.find("没有生成物") != std::string::npos) LIFT_SKIP("AOT 生成物不在场（缺 clang / 执行源）");
        if (ls.err.find("拒绝启动") != std::string::npos) {
            LIFT_SKIP("GPU / 窗口初始化不成立（headless 也要 GPU device）");
        }
        // ---- 判据 A：readout 说"链真的走到 FIELD"（不含 calls 的确切数）----
        const bool reached_field = ls.out.find("plugin    : ") != std::string::npos &&
                                   ls.out.find("entry=default") != std::string::npos &&
                                   ls.out.find("callbacks=yes") != std::string::npos &&
                                   ls.out.find("params=slot=5") != std::string::npos &&
                                   ls.out.find("run=done") != std::string::npos &&
                                   ls.out.find("run=failed") == std::string::npos &&
                                   ls.err.find("run failed") == std::string::npos &&
                                   ls.err.find("load-save: gate timeout") == std::string::npos &&
                                   ls.err.find("FIELD") != std::string::npos;
        if (!reached_field) {
            std::fprintf(stderr, "  stdout:\n%s\n  stderr:\n%s\n", ls.out.c_str(), ls.err.c_str());
        }
        LIFT_CHECK(reached_field);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(countOccurrences(ls.out, "plugin    : ")), 1);
        // ---- 判据 B：`calls ∈ {6,7}`（D2c-2）----
        //   6 = start + TITLE/SAVE/row/SBUNKI/FIELD（点完 640,323 直接 FIELD）；
        //   7 = 上面那条 + **SBUNKI 0x0AF8 第二道确认**（再点 690,323 才到 FIELD；mac headless 实测）。
        //   第二道确认**也是** D-C2C-3 三选一终端的一部分 ⇒ 两条都是合法终态。⛔ 不是"任意 calls"：
        //   只接受这两个数；8+/更少 ⇒ 红。
        const bool calls6 = ls.out.find("calls=6") != std::string::npos;
        const bool calls7 = ls.out.find("calls=7") != std::string::npos;
        if (!(calls6 || calls7)) {
            std::fprintf(stderr, "  stdout:\n%s\n  stderr:\n%s\n", ls.out.c_str(), ls.err.c_str());
        }
        LIFT_CHECK(calls6 || calls7);
    }
    return lift_test::finish("plugin-headless");
}
