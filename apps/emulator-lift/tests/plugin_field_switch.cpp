// emulator-lift · `field-point-visible.js` 是**开关**，不是"自驱复现脚本"（父级裁决 2026-11-25）
//
// 判据（红了意味着什么）：
//   ① **静态契约**（剥注释后只看**代码**；零资产、无子进程）：
//      · 没有输入写面（`ctx.input`）⇒ 不会点鼠标；
//      · 没有读档链（`loadSave` / `bootToTitle` / `save.js` / `boot-to-title.js` / `chains.js`）⇒ 不会自己读档；
//      · 不结束 Run（`ctx.finish` / `ctx.quit`）⇒ 常驻（正常路径永不收口，进程按 `--time-limit` / 关窗结束）；
//      · 还在用**位置触发**那条链（`afterPosition` + `reenter` + 两个 setmap 区间
//        `FIELD_SETMAP_A_LO` / `FIELD_SETMAP_B_LO` + `forceFieldPointsVisible` +
//        `static` 的**点位绘制脚本入口** `DRAWOBJ_ENTRY_LO/HI` / `fp:draw` /
//        `clearFieldStaticSkip`；
//        ⛔ 没有 `afterDelay` / `poll` / `FIELD_MAIN_LOOP` —— 判据是已知脚本位置，不是轮询）。
//      红了 ⇒ 这个"开关"又开始驱动输入 / 读档 / 自己收口了，或者 `static` 那条链被悄悄拆掉了。
//   ② **端到端常驻**（真子进程 `--present headless --plugin field-point-visible`，**无** `--input-plan` /
//      `--control-root` —— 它们与 `--plugin` 互斥）：
//      · 进程按 `--time-limit` 收口：readout `run=waiting`（≠ done / finished / failed）+ 摘要
//        `stop      : time-limit` + exit 0；`calls>=1`（start）⇒ 插件**常驻**（位置触发只在玩家进
//        FIELD 的 setmap 时 fire；headless 停在标题 ⇒ 没有第二次回调）；
//      · stdout+stderr **没有** `boot:` / `load:` / `load-save:` / `[input-script]` / `input-plan:` 行
//        ⇒ 没有自驱到 TITLE、没有读档、没有输入计划；
//      · 首行 `field-points:` 日志带 `on=0`（缺省关），且**没有** `forced=` / `setmap`
//        ⇒ "关着的时候什么也不做"是**可读的**（不是靠"没看见"猜）。
//   ★ 缺执行源 / 安装树 / GPU / 宿主字体 ⇒ `LIFT_SKIP(77)`（与 `plugin-headless` 同一条口径：输入假定在场）。
//     ★ 静态那一段在 skip **之前**跑完 ⇒ 缺资产不会把"契约"整段跳掉（失败仍会 exit 1，不会被 77 盖住）。
//   ★ 档位 = **pure**（`add_test` 在顶层 `if(LIFT_BUILD_TESTS)` 里 ⇒ `cmakeTestTier()` 把该 if 当透明，
//     `AGENTS.md` §12.4 判据 = 注册位置）。端到端那一段要安装树 —— 缺了 skip，⛔ 不是"这档变 assets"。
//
// ⛔ 本用例**不**断言画面 / 游戏业务（"点位真的画出来了"属 FIELD 复现，归 `scenario-save10-field.js`）。
#include <chrono>
#include <cstdint>
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

// 剥掉 `//` 行注释与 `/* … */` 块注释，只留**代码**。
// ★ 为什么要剥：文件头注释里**必须**能写"⛔ 不 import boot-to-title.js / save.js / chains.js"，
//   否则禁令与判据会互相打架（守卫只能靠注释说明，而注释里不许出现被禁的名字 = 荒谬）。
// ★ ⛔ 不解析字符串字面量：代码里的字符串出现这些名字同样算坏（那是"换了个马甲继续驱动输入"）。
std::string stripComments(const std::string& src) {
    std::string out;
    out.reserve(src.size());
    bool line_comment = false;
    bool block_comment = false;
    for (std::size_t i = 0; i < src.size(); ++i) {
        const char c = src[i];
        const char next = i + 1 < src.size() ? src[i + 1] : '\0';
        if (line_comment) {
            if (c == '\n') {
                line_comment = false;
                out.push_back(c);
            }
            continue;
        }
        if (block_comment) {
            if (c == '*' && next == '/') {
                block_comment = false;
                ++i;
            }
            continue;
        }
        if (c == '/' && next == '/') {
            line_comment = true;
            ++i;
            continue;
        }
        if (c == '/' && next == '*') {
            block_comment = true;
            ++i;
            continue;
        }
        out.push_back(c);
    }
    return out;
}

bool readWholeFile(const fs::path& path, std::string* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

std::size_t countOccurrences(const std::string& text, const std::string& needle) {
    std::size_t n = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) {
        ++n;
    }
    return n;
}

// readout 的 `calls=`（找不到 / 不是数字 ⇒ -1；⛔ 不把"没找到"当 0）。
// ★ 必须**从 `plugin    : ` 那一行**往后找：启动读数里的 `services  : … calls=yes …` 也含
//   `calls=` 这个子串（但它不是计数）—— 直接从 0 找会拿到 `yes` ⇒ 永远判 -1。
std::int64_t readoutCalls(const std::string& out) {
    const std::size_t line = out.find("plugin    : ");
    if (line == std::string::npos) return -1;
    const std::string key = "calls=";
    std::size_t at = out.find(key, line);
    if (at == std::string::npos) return -1;
    at += key.size();
    std::int64_t value = 0;
    bool any = false;
    while (at < out.size() && out[at] >= '0' && out[at] <= '9') {
        value = value * 10 + (out[at] - '0');
        ++at;
        any = true;
    }
    return any ? value : -1;
}

// 第一行含 `field-points:` 的 stderr 行（⛔ 不编：没有就返回空串，由断言点名）。
std::string firstFieldPointsLine(const std::string& err) {
    const std::size_t at = err.find("field-points:");
    if (at == std::string::npos) return std::string();
    std::size_t begin = err.rfind('\n', at);
    begin = begin == std::string::npos ? 0 : begin + 1;
    std::size_t end = err.find('\n', at);
    if (end == std::string::npos) end = err.size();
    return err.substr(begin, end - begin);
}

// 一行里**不许**出现的自驱痕迹（`boot:` / `load:` 是链的具名状态；后三个是别的驱动器）。
const char* const kForbiddenOutput[] = {"boot:", "load:", "load-save:", "[input-script]", "input-plan:"};

}  // namespace

int main(int argc, char** argv) {
    using namespace lift_test;

    // =================================================================================
    // ① 静态契约（零资产；缺资产的机器上也跑）
    // =================================================================================
    g_current_case = "field-switch/static-contract";
    const fs::path plugin = fs::path(LIFT_PLUGIN_DIR) / "field-point-visible.js";
    std::string source;
    if (!readWholeFile(plugin, &source)) {
        LIFT_CHECK(false);
        std::fprintf(stderr, "  读不到插件：%s\n", pathToUtf8(plugin).c_str());
        return lift_test::finish("plugin-field-switch");
    }
    LIFT_CHECK(!source.empty());
    const std::string code = stripComments(source);
    LIFT_CHECK(!code.empty());
    // 剥注释**必须**真的剥掉了东西（否则那两段注释在 `code` 里 ⇒ 下面的禁令检查是假绿）。
    LIFT_CHECK(code.size() < source.size());
    // 禁令：输入写面 / 读档链 / 自己收口。
    LIFT_CHECK(code.find("ctx.input") == std::string::npos);
    LIFT_CHECK(code.find("loadSave") == std::string::npos);
    LIFT_CHECK(code.find("bootToTitle") == std::string::npos);
    LIFT_CHECK(code.find("boot-to-title.js") == std::string::npos);
    LIFT_CHECK(code.find("save.js") == std::string::npos);
    LIFT_CHECK(code.find("chains.js") == std::string::npos);
    LIFT_CHECK(code.find("ctx.finish") == std::string::npos);
    LIFT_CHECK(code.find("ctx.quit") == std::string::npos);
    // 正向：该写的那条链还在（否则"禁令全过"可以靠一个空文件达成）。
    LIFT_CHECK(code.find("forceFieldPointsVisible") != std::string::npos);
    // ★ `static`（缺省 1）：**`DRAWOBJ.BIN` 入口**的位置触发 + 静态判据函数 + 设置键，都得在代码里。
    //   红了 = "进图显示静态"这条链被拆了（用户裁决 2026-11-26）。
    //   ★ 为什么是 DRAWOBJ 而不是 RENDERMAP 的调用点：4 处 `call-script 5117` 都从 DRAWOBJ 进
    //     （`.tmp/field-idle/sites.md`），一条钩子覆盖全部调用者。
    LIFT_CHECK(code.find("clearFieldStaticSkip") != std::string::npos);
    LIFT_CHECK(code.find("DRAWOBJ.BIN") != std::string::npos);
    LIFT_CHECK(code.find("DRAWOBJ_ENTRY_LO") != std::string::npos);
    LIFT_CHECK(code.find("DRAWOBJ_ENTRY_HI") != std::string::npos);
    LIFT_CHECK(code.find("\"fp:draw\"") != std::string::npos);
    LIFT_CHECK(code.find("\"static\"") != std::string::npos);
    // ★ 判据是 setmap 的**已知脚本位置** ⇒ 三条位置触发（reenter），⛔ 不是轮询。
    LIFT_CHECK(code.find("afterPosition") != std::string::npos);
    LIFT_CHECK(code.find("reenter") != std::string::npos);
    LIFT_CHECK(code.find("FIELD_SETMAP_A_LO") != std::string::npos);
    LIFT_CHECK(code.find("FIELD_SETMAP_B_LO") != std::string::npos);
    LIFT_CHECK(code.find("FIELD_MAP_INSTANCE_GLOBAL_ID") != std::string::npos);
    LIFT_CHECK(code.find("FIELD_MAIN_LOOP") == std::string::npos);
    LIFT_CHECK(code.find("afterDelay") == std::string::npos);
    LIFT_CHECK(code.find("DEFAULT_POLL_MS") == std::string::npos);
    // ★ `poll` 这个子串在代码里出现 ⇒ 有人把"每拍重读"的轮询又接回来了（注释已剥 ⇒ 不会误伤）。
    LIFT_CHECK(code.find("poll") == std::string::npos);
    // 日志前缀是外部可判面（下面 ② 与文档都按它抓）。
    LIFT_CHECK(source.find("field-points:") != std::string::npos);
    const std::size_t forbidden_tokens = sizeof(kForbiddenOutput) / sizeof(kForbiddenOutput[0]) + 8;
    std::printf("plugin-field-switch: static checks done（source=%zu B / code=%zu B / %zu 个禁令 token；"
                "failures=%d）\n",
                source.size(), code.size(), forbidden_tokens, g_failures);

    // =================================================================================
    // ② 端到端常驻（要安装树 / 执行源 / GPU / 字体；缺了 skip —— 静态那一段已经跑完）
    // =================================================================================
    g_current_case = "field-switch/resident-headless";
    if (argc < 4) {
        LIFT_SKIP("需要 CTest 传 <emulator-lift-aot> <AGE.EXE__dumped.sectfix.EXE> <安装树>（见 CMakeLists）");
    }
    const fs::path exe = fs::path(argv[1]);
    const fs::path age_exe = fs::path(argv[2]);
    const fs::path game_dir = fs::path(argv[3]);
    if (!fs::exists(exe)) LIFT_SKIP("emulator-lift-aot 不在场");
    if (!fs::exists(age_exe)) LIFT_SKIP("执行源（corpus/assets/age/AGE.EXE__dumped.sectfix.EXE）不在场");
    if (!fs::exists(game_dir)) LIFT_SKIP("安装树（--game-dir）不在场");

    std::error_code ec;
    const fs::path root = fs::temp_directory_path(ec) /
                          ("lift-plugin-field-switch-" + std::to_string(lift_test::processId()));
    fs::remove_all(root, ec);
    fs::create_directories(root / "appdata", ec);
    LIFT_CHECK(!ec);

    // ★ 没有 `--plugin-param`（缺省 = `on 0`）；appdata 里也**没有** plugin-config.json（缺省照旧）。
    //   ⛔ 不给 `--input-plan` / `--control-root`（它们与 `--plugin` 互斥，见 `src/plugin/readings.h`）。
    const std::vector<std::string> args = {
        "--exe",
        pathToUtf8(age_exe),
        "--game-dir",
        pathToUtf8(game_dir),
        "--appdata",
        pathToUtf8(root / "appdata"),
        "--present",
        "headless",
        "--skip-logo",
        "--audio",
        "off",
        "--time-limit",
        "20",
        "--plugin",
        pathToUtf8(plugin),
    };
    ChildOptions opts;
    opts.cwd = root;
    opts.timeout = std::chrono::milliseconds(180000);
    const ChildResult r = runChild(exe, args, opts);
    LIFT_CHECK(r.started);
    if (!r.started) return lift_test::finish("plugin-field-switch");
    std::printf("plugin-field-switch: exit=%d timed_out=%d\n", r.exit_code, r.timed_out ? 1 : 0);

    if (r.timed_out) {
        LIFT_CHECK(false);
        std::fprintf(stderr, "  stdout:\n%s\n  stderr:\n%s\n", r.out.c_str(), r.err.c_str());
        return lift_test::finish("plugin-field-switch");
    }
    // ---- 缺资产 / GPU / 字体 ⇒ skip（与 `plugin-headless` 同一条口径；⛔ 不假红）----
    if (r.err.find("宿主字体不可用") != std::string::npos) LIFT_SKIP("缺宿主字体（LIFT_HOST_FONT_PATH）");
    if (r.err.find("没有生成物") != std::string::npos) LIFT_SKIP("AOT 生成物不在场（缺 clang / 执行源）");
    if (r.err.find("拒绝启动") != std::string::npos) {
        LIFT_SKIP("GPU / 窗口初始化不成立（headless 也要 GPU device）");
    }

    // ---- 常驻：进程按 `--time-limit` 收口（⛔ 不是 done / finished / failed）----
    if (r.exit_code != 0 || r.out.find("run=waiting") == std::string::npos) {
        std::fprintf(stderr, "  stdout:\n%s\n  stderr:\n%s\n", r.out.c_str(), r.err.c_str());
    }
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(r.exit_code), 0);
    LIFT_CHECK(r.out.find("plugin    : ") != std::string::npos);
    LIFT_CHECK(r.out.find("field-point-visible.js") != std::string::npos);
    LIFT_CHECK(r.out.find("entry=default") != std::string::npos);
    LIFT_CHECK(r.out.find("callbacks=yes") != std::string::npos);
    LIFT_CHECK(r.out.find("run=waiting") != std::string::npos);   // ★ 常驻（Run 还没收口）
    LIFT_CHECK(r.out.find("run=failed") == std::string::npos);
    LIFT_CHECK(r.out.find("run=done") == std::string::npos);
    LIFT_CHECK(r.out.find("run=finished") == std::string::npos);
    LIFT_CHECK(r.out.find("stop      : time-limit") != std::string::npos);
    // readout 只一行（⛔ 不是每帧 / 每读点一行；`AGENTS.md` §12.8 第 3 条）。
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(countOccurrences(r.out, "plugin    : ")), 1);
    const std::int64_t calls = readoutCalls(r.out);
    // ★ 位置触发只在玩家进 FIELD 时 fire；headless 停在标题 ⇒ 只有 start 那一次回调。
    if (calls < 1) std::fprintf(stderr, "  calls=%lld（至少要有 start）\n", static_cast<long long>(calls));
    LIFT_CHECK(calls >= 1);

    // ---- 不自驱：没有 boot / load / input 的行（stdout 与 stderr 都查）----
    for (const char* token : kForbiddenOutput) {
        const bool in_out = r.out.find(token) != std::string::npos;
        const bool in_err = r.err.find(token) != std::string::npos;
        if (in_out || in_err) {
            std::fprintf(stderr, "  自驱痕迹 `%s`：stdout=%d stderr=%d\n", token, in_out ? 1 : 0,
                         in_err ? 1 : 0);
        }
        LIFT_CHECK(!in_out);
        LIFT_CHECK(!in_err);
    }

    // ---- `on=0` 是**可读的**：首行 `field-points:` 明写 on=0，且一次都没施加过 ----
    const std::string first_line = firstFieldPointsLine(r.err);
    if (first_line.empty() || first_line.find("on=0") == std::string::npos) {
        std::fprintf(stderr, "  stderr:\n%s\n", r.err.c_str());
    }
    LIFT_CHECK(!first_line.empty());
    LIFT_CHECK(first_line.find("on=0") != std::string::npos);
    LIFT_CHECK(r.err.find("forced=") == std::string::npos);          // 关着 ⇒ 一个槽都没写
    LIFT_CHECK(r.err.find("field-points: setmap") == std::string::npos);  // 没人读档 ⇒ 从没到过 setmap
    LIFT_CHECK(r.err.find("re-applied") == std::string::npos);

    return lift_test::finish("plugin-field-switch");
}
