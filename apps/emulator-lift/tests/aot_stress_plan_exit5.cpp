// emulator-lift · **判据 4 的自检**（`LIFT_STRESS` 档）：`--input-plan` 的完成度判据真的会红
//
// 判据（红了意味着什么）：一份**永不满足**的配方门（夹具 `tests/fixtures/boot-title-exit-never.plan`）
// 交给 `emulator-lift-aot --input-plan` ⇒ 宿主进程必须
//   ① **exit 5**（= `recipe.h` 的 `kRecipeIncompleteExitCode`，由 `aot_main` 在 `plan_outcome != Done`
//     时返回；⛔ 不是 exit 0、⛔ 不是 exit 2 的解析失败）；
//   ② stderr 上**点名门**：`<夹具文件名>:<门那一行>`（行号由本用例从夹具里读出来，⛔ 不写死）；
//      同时把那个门本身（`await position TITLE`）打出来；
//   ③ stderr 上有 `input-plan: FAIL` 与 `pending=<非 0>`（"没走完"的形状，⛔ 不是"解析失败"）。
//   ⇒ 红了意味着：配方没走完这件事在 **CLI 层**又变回假绿面（只打一行 stderr 仍 exit 0）、
//     或者失败报告不再点名"哪一行 / 哪个门"，或者完成度判据本身被改坏。
//
// ★ 为什么这条要存在（本单 `REQ-01M7HQJEEENYDJ91FB5DAC4K23` 的判据 4）：规则级守卫
//   （`input-plan-read-points` 的 `input-plan/completion`、`recipe-driver-parity` 的用例 3）
//   已经钉住 `recipeOutcomeExitCode` 这个**函数**；它们证明不了"`aot_main` 那个**调用点**接上了"。
//   本条是那条规则的**端到端接线**：真 exe + 真配方文件 + 真进程退出码。
//   ⇒ 同类变异（把 `aot_main` 的 `return lift::recipeOutcomeExitCode(plan_outcome);` 改成 0）
//     只有**这一条**能抓（`tools/mutate-check.mjs` 的 `aot-stress-plan-exit5` 条目）。
//
// ★ 为什么 `--max-frames 1`：只要解析成功、门永不成立，跑一帧就够把"没走完"暴露出来
//   （实测 ~3–5 s；不加它要等到 `--time-limit`）。⛔ 它**不改变**判据：门在 TITLE 的退出路径段，
//   而本配方从不点 Quit ⇒ 门在结构上不可能成立，无论跑多久都该 exit 5。
//   ⚠ 反过来注意：因为 `--max-frames 1` 会在**任何**门之前停，"把门换成会命中的区间"这条路
//     在本用例里**不会**变绿（实测 exit 仍是 5）⇒ 能红的自证走
//     `tools/mutate-check.mjs --match 'aot-stress-plan-exit5'`（改 `aot_main` 的调用点）。
//
// ★ 为什么在 `LIFT_STRESS` 档：它**真跑原版安装树**（`links/raw` + 真 boot），是本档"能红"的负控
//   （`AGENTS.md` §12.4：档位判据 = 注册位置 / 用途，⛔ 不看"资产在不在"）。
// ★ 缺输入（GPU / 字体 / 生成物）⇒ `LIFT_SKIP(77)` + 原因（先例 `plugin_e2e`；⛔ 不假绿、⛔ 不假红）。
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

/** 夹具里 `await position` 那一行是第几行（1 基）；0 = 没找到。 */
int gateLineOf(const std::string& text) {
    int line_no = 0;
    std::size_t at = 0;
    while (at <= text.size()) {
        const std::size_t nl = text.find('\n', at);
        const std::size_t end = nl == std::string::npos ? text.size() : nl;
        ++line_no;
        // `#` 注释行不算门（本夹具的头注释里就写着 `await position` 这个词）。
        const std::string line = text.substr(at, end - at);
        const std::size_t hash = line.find('#');
        const std::string code = hash == std::string::npos ? line : line.substr(0, hash);
        if (code.find("await position") != std::string::npos) return line_no;
        if (nl == std::string::npos) break;
        at = nl + 1;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace lift_test;

    if (argc < 5) {
        LIFT_SKIP("需要 CTest 传 <emulator-lift-aot> <执行源> <links/raw> <fixture .plan>（见 CMakeLists）");
    }
    const fs::path aot_exe = argv[1];
    const fs::path age_exe = argv[2];
    const fs::path raw_dir = argv[3];
    const fs::path fixture = argv[4];

    std::error_code ec;
    if (!fs::exists(aot_exe, ec)) LIFT_SKIP("emulator-lift-aot 不在场");
    if (!fs::exists(age_exe, ec)) LIFT_SKIP("执行源（corpus/assets/age/AGE.EXE__dumped.sectfix.EXE）不在场");
    if (!fs::exists(raw_dir / "AGE.EXE", ec)) LIFT_SKIP("links/raw/AGE.EXE 不在场（原版安装树）");
    if (!fs::exists(fixture, ec)) LIFT_SKIP("夹具 tests/fixtures/boot-title-exit-never.plan 不在场");

    // 门在第几行**从夹具里读**（⛔ 不写死行号：夹具一改，写死的那种断言就变成"绿得没证据"）。
    const int gate_line = gateLineOf(readAll(fixture));
    if (gate_line == 0) {
        failf(__FILE__, __LINE__, "夹具里没有 `await position` 门：%s", pathToUtf8(fixture).c_str());
        return finish("aot-stress-plan-exit5");
    }

    const fs::path root = fs::temp_directory_path(ec) /
                          ("lift-aot-stress-plan-exit5-" + std::to_string(lift_test::processId()));
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    LIFT_CHECK(!ec);
    const fs::path appdata = root / "appdata";
    fs::create_directories(appdata, ec);

    g_current_case = "aot-stress/plan-exit5";
    const std::vector<std::string> args = {
        "--exe",       pathToUtf8(age_exe),
        "--game-dir",  pathToUtf8(raw_dir),
        "--appdata",   pathToUtf8(appdata),
        "--present",   "headless",
        "--skip-logo", "--audio", "off",
        "--max-frames", "1",
        "--input-plan", pathToUtf8(fixture),
    };
    ChildOptions opts;
    opts.cwd = root;
    opts.timeout = std::chrono::milliseconds(180000);
    const ChildResult r = runChild(aot_exe, args, opts);
    LIFT_CHECK(r.started);
    if (!r.started) {
        fs::remove_all(root, ec);
        return finish("aot-stress-plan-exit5");
    }
    std::printf("aot-stress-plan-exit5: exit=%d timed_out=%d gate_line=%d\n", r.exit_code,
                r.timed_out ? 1 : 0, gate_line);

    // ---- 缺输入 ≠ 接线坏了（先例 plugin_e2e；⛔ 不把"没跑成"记成红或绿）----
    if (r.err.find("拒绝启动") != std::string::npos)
        LIFT_SKIP("GPU / 窗口初始化不成立（headless 也要 GPU device）");
    if (r.err.find("没有生成物") != std::string::npos) LIFT_SKIP("AOT 生成物不在场（缺 clang / 执行源）");
    if (r.err.find("宿主字体不可用") != std::string::npos) LIFT_SKIP("缺宿主字体（LIFT_HOST_FONT_PATH）");
    if (r.timed_out) {
        failf(__FILE__, __LINE__, "子进程 180 s 未结束（配方没走完也不该挂住）");
        fs::remove_all(root, ec);
        return finish("aot-stress-plan-exit5");
    }

    const std::string named = fixture.filename().string() + ":" + std::to_string(gate_line);
    if (r.exit_code != 5 || !contains(r.err, named)) {
        std::fprintf(stderr, "--- stdout (tail) ---\n%s\n--- stderr (tail) ---\n%s\n",
                     r.out.substr(r.out.size() > 3000 ? r.out.size() - 3000 : 0).c_str(),
                     r.err.substr(r.err.size() > 3000 ? r.err.size() - 3000 : 0).c_str());
    }
    // ---- ① 完成度判据：没走完 ⇒ exit 5（⛔ 0 = 假绿；⛔ 2 = 解析失败，不是本条要的形状）----
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(r.exit_code), 5);
    // ---- ② 点名"哪一行 / 哪个门" ----
    LIFT_CHECK(contains(r.err, named));
    LIFT_CHECK(contains(r.err, "await position TITLE"));
    // ---- ③ "没走完"的形状（⛔ 不是别的失败）----
    LIFT_CHECK(contains(r.err, "input-plan: FAIL"));
    LIFT_CHECK(contains(r.err, "pending="));
    LIFT_CHECK(!contains(r.err, "pending=0"));
    // 这条 run 里不该有**响亮错**（decode / 派发表缺口会把"完成度"的结论污染掉）。
    // ⚠ 判据是 `emulator-lift-aot: ⛔` 这个**前缀**（`aot_main` 响亮错的唯一拼法），⛔ 不是裸 `⛔`：
    //    正常读数里 `⛔` 也出现在**解释性文案**里（本用例的失败报告自己就写着 `⛔ 不是成功`，
    //    停因摘要/`lift_dyn` 行里也有 `⛔ 不进闭式` 这类）⇒ 裸判会假红。
    LIFT_CHECK(!contains(r.err, "emulator-lift-aot: ⛔"));
    LIFT_CHECK(!contains(r.err, "派发表里没有的 EA"));
    LIFT_CHECK(!contains(r.err, "decode-error"));

    fs::remove_all(root, ec);
    return finish("aot-stress-plan-exit5");  // 与 CTest 注册名一致
}
