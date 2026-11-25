// emulator-lift · **路径 2（打开 FELLOW，6 次立绘切换）**的端到端读数面（`LIFT_STRESS` 档）
//
// 判据（红了意味着什么）：真库驱动 `scripts/plugins/scenario-fellow-switch.js`（S1 起入口在插件**根**，
// 库在 `lib/`；`--present headless --skip-logo --audio off --time-limit 240 --profile`）交给
// `emulator-lift-aot --plugin` ⇒ 宿主进程必须
//   ① **exit 0**；
//   ② 插件 readout 是 **`run=finished exit=0`**（⛔ 不是 `exit=5 reason=…`；也不许"只靠
//      `--time-limit` 收尾"——`--plugin` 路的失败**不**改宿主退出码，所以判据必须读 readout）；
//   ③ stdout 里 **`=== profile window:` 恰好 6 条**，且 6 个切换窗口标签逐字齐全
//      （`s1-to-avaro` / `s1-to-fia` / `s2-…` / `s3-to-fia`）——这是"6 次切换真的都走完了"的读数面；
//      ⛔ **不断言任何 ms / ns 数字**（AGENTS.md §12.6：读数进文档，不进 test）；
//   ④ 没有 `⛔` / `派发表里没有的 EA` / `decode-error`。
//   ⇒ 红了意味着：打开 FELLOW 这条端到端路径断了（载入前缀 / 右键进 FELLOW / 6 个切换窗口之一）。
//
// ★ 为什么读 out 而不只读退出码：`src/plugin/plugin_host.cpp` 的合同是"插件失败 ⇒ receipt + 一行，
//   ⛔ 不让核心 exit 5"（`ctx.finish(5)` 只写 readout）⇒ **只看退出码会把失败读成绿**。
//   注：本档没有 `--input-plan`，所以没有 `pending=` 这行；`run=finished exit=0` 是它的等价面。
//
// ★ 能红的改法（⛔ 不动 `scripts/plugins/lib/**` 的共用件）：把 `script-positions.js` 的
//   `FELLOW_READ_INPUT_LO/HI` 换成 FELLOW 的**轮询体**区间（`script-positions.js` 已记的负对照：
//   门命中但点击被吞）⇒ 切换门超时 ⇒ `ctx.finish(5)` ⇒ readout `exit=5` ⇒ ②③ 红。
//   实测记录：本用例第一次跑（2026-11-24）在**载入前缀**就红了 —— `loadSave(9)` 的翻页门
//   `load-save: page not reached (target 0, still 1)`（见报告 `## 未做`：路径 3 的翻页是既有缺口）。
//
// ★ **2026-11-24 曾登记 `DISABLED TRUE`**（`CMakeLists.txt` 的 stress 块）：当时三次独立 run 都红在
//   两个**既有**输入缺口上 —— ① `REQ-01M86VRRTPY8W9RP1SFDT0N200`（`loadSave(9)` 翻页点击会丢）；
//   ② `REQ-01M86VRT1YP53CJS0M885FCCJM`（第一个 `settle` 回调 `js_cpu_us=2217 > 2000`）。
//   2026-11-25 两张单都收口（翻页窗 60→1200 读点；`ctx.profile.end` 的宿主成本走"宿主调用暂停"）
//   ⇒ **`DISABLED` 已删**。⛔ 再红就当一个真回归修，不要再把它藏起来、⛔ 也不要改成 skip/放宽断言。
//
// ★ 为什么在 `LIFT_STRESS` 档：真跑（`saves/` + 真安装树 + 240 s 上限）⇒ 只能进"唯一允许跑真游戏"
//   的那档（`AGENTS.md` §12.4）。
// ★ `saves/` 是**只读**复现素材 ⇒ 复制进 `.tmp/` 下的私有 `--appdata`（照 `scripts/clean-appdata.sh`；
//   ⛔ 不就地写）。
// ★ 缺输入（GPU / 字体 / 生成物 / 存档）⇒ `LIFT_SKIP(77)`（先例 `plugin_e2e`）。
#include <chrono>
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

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

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

    if (argc < 6) {
        LIFT_SKIP("需要 CTest 传 <emulator-lift-aot> <执行源> <安装树> <saves/> <scenario-fellow-switch.js>"
                  "（见 CMakeLists）");
    }
    const fs::path aot_exe = argv[1];
    const fs::path age_exe = argv[2];
    const fs::path game_dir = argv[3];
    const fs::path saves_dir = argv[4];
    const fs::path plugin_js = argv[5];

    std::error_code ec;
    if (!fs::exists(aot_exe, ec)) LIFT_SKIP("emulator-lift-aot 不在场");
    if (!fs::exists(age_exe, ec)) LIFT_SKIP("执行源（corpus/assets/age/AGE.EXE__dumped.sectfix.EXE）不在场");
    if (!fs::exists(game_dir, ec)) LIFT_SKIP("安装树（--game-dir）不在场");
    if (!fs::exists(plugin_js, ec)) LIFT_SKIP("驱动 scripts/plugins/scenario-fellow-switch.js 不在场");
    if (!fs::exists(saves_dir / "SAVE09.DAT", ec)) LIFT_SKIP("saves/SAVE09.DAT 不在场（本驱动要槽 9）");
    if (!fs::exists(saves_dir / "SAVE.DAT", ec)) LIFT_SKIP("saves/SAVE.DAT 不在场（读档页号的真源）");

    // ---- 私有 appdata：`saves/` 只读 ⇒ 复制（照 clean-appdata.sh 的取舍：SAVE*.DAT/STH + SAVE.DAT/RT.DAT）----
    const fs::path root = fs::temp_directory_path(ec) /
                          ("lift-aot-stress-fellow-" + std::to_string(lift_test::processId()));
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    LIFT_CHECK(!ec);
    const fs::path appdata = root / "appdata";
    const fs::path guest_save_dir =
        appdata / "Local" / "Eushully" / fs::path(std::u8string(u8"天結いキャッスルマイスター")) / "SAVE";
    fs::create_directories(guest_save_dir, ec);
    LIFT_CHECK(!ec);
    {
        std::error_code copy_ec;
        std::size_t copied = 0;
        for (fs::directory_iterator it(saves_dir, copy_ec);
             !copy_ec && it != fs::directory_iterator(); it.increment(copy_ec)) {
            if (!it->is_regular_file(copy_ec) || copy_ec) continue;
            const std::string name = it->path().filename().string();
            // `SAVE.BAK` / `$$AGE.TMP` 是游戏自己的中间件（clean-appdata.sh 也删）；`README.md` 不是存档。
            if (name == "README.md" || name == "SAVE.BAK" || name == "$$AGE.TMP") continue;
            fs::copy_file(it->path(), guest_save_dir / it->path().filename(),
                          fs::copy_options::overwrite_existing, copy_ec);
            if (!copy_ec) ++copied;
        }
        LIFT_CHECK(!copy_ec);
        LIFT_CHECK(copied > 0);
        LIFT_CHECK(fs::exists(guest_save_dir / "SAVE09.DAT", ec));
    }

    g_current_case = "aot-stress/fellow-switch";
    const std::vector<std::string> args = {
        "--exe",        pathToUtf8(age_exe),
        "--game-dir",   pathToUtf8(game_dir),
        "--appdata",    pathToUtf8(appdata),
        "--present",    "headless",
        "--skip-logo",  "--audio", "off",
        "--time-limit", "240",
        "--profile",
        "--plugin",     pathToUtf8(plugin_js),
    };
    ChildOptions opts;
    opts.cwd = root;
    opts.timeout = std::chrono::milliseconds(360000);  // CTest 侧 420 s 兜底
    const ChildResult r = runChild(aot_exe, args, opts);
    LIFT_CHECK(r.started);
    if (!r.started) {
        fs::remove_all(root, ec);
        return finish("aot-stress-fellow-switch");
    }
    std::printf("aot-stress-fellow-switch: exit=%d timed_out=%d windows=%zu\n", r.exit_code,
                r.timed_out ? 1 : 0, countOccurrences(r.out, "=== profile window:"));

    if (r.err.find("拒绝启动") != std::string::npos)
        LIFT_SKIP("GPU / 窗口初始化不成立（headless 也要 GPU device）");
    if (r.err.find("没有生成物") != std::string::npos) LIFT_SKIP("AOT 生成物不在场（缺 clang / 执行源）");
    if (r.err.find("宿主字体不可用") != std::string::npos) LIFT_SKIP("缺宿主字体（LIFT_HOST_FONT_PATH）");
    if (r.timed_out) {
        failf(__FILE__, __LINE__, "子进程 360 s 未结束（--time-limit 240 早该收尾）");
        fs::remove_all(root, ec);
        return finish("aot-stress-fellow-switch");
    }

    // 失败时把 readout + 关键行打出来（ctest --output-on-failure 能直接读）。
    const bool readout_ok = contains(r.out, "run=finished exit=0");
    if (r.exit_code != 0 || !readout_ok) {
        std::fprintf(stderr, "--- plugin readout ---\n");
        for (std::size_t at = r.out.find("plugin    : "); at != std::string::npos;
             at = r.out.find("plugin    : ", at + 1)) {
            const std::size_t nl = r.out.find('\n', at);
            std::fprintf(stderr, "%s\n", r.out.substr(at, nl - at).c_str());
        }
        std::fprintf(stderr, "--- stderr (tail) ---\n%s\n",
                     r.err.substr(r.err.size() > 3000 ? r.err.size() - 3000 : 0).c_str());
    }
    // ---- ① 宿主进程正常收尾（`--time-limit` 也是"正常结局" ⇒ 这条**单独不足以**判成功）----
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(r.exit_code), 0);
    // ---- ② 插件 readout：驱动自己走完了（这才是 `--plugin` 路的可判面）----
    LIFT_CHECK(readout_ok);
    LIFT_CHECK(!contains(r.out, "run=failed"));
    // ---- ③ 6 个切换窗口，标签逐字齐全（⛔ 不判 ms）----
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(countOccurrences(r.out, "=== profile window:")), 6);
    for (const char* label : {"s1-to-avaro", "s1-to-fia", "s2-to-avaro", "s2-to-fia", "s3-to-avaro",
                              "s3-to-fia"}) {
        LIFT_CHECK(contains(r.out, std::string("=== profile window: ") + label + " ==="));
    }
    // ---- ④ 没有响亮错 ----
    // ⚠ 判据是 `emulator-lift-aot: ⛔` 这个**前缀**（`aot_main` 响亮错的唯一拼法），⛔ 不是裸 `⛔`：
    //    正常读数里 `⛔` 也在**解释性文案**里出现（如 `lift_dyn` 行的 `⛔ 不进闭式`）⇒ 裸判会假红。
    LIFT_CHECK(!contains(r.err, "emulator-lift-aot: ⛔"));
    LIFT_CHECK(!contains(r.err, "派发表里没有的 EA"));
    LIFT_CHECK(!contains(r.err, "decode-error"));

    fs::remove_all(root, ec);
    return finish("aot-stress-fellow-switch");  // 与 CTest 注册名一致
}
