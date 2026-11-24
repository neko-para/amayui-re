// emulator-lift · **路径 1（boot → TITLE）**的端到端读数面（`LIFT_STRESS` 档）
//
// 判据（红了意味着什么）：配方 `scripts/recipes/boot-title.plan`（门 + `quit`，中间一个
// `boot-title` 区间 profiler 窗）交给 `emulator-lift-aot --input-plan` ⇒ 宿主进程必须
//   ① **exit 0**；
//   ② `input-plan: … pending=0`（配方四步全部被消费 —— 完成度判据的**正例**面）；
//   ③ 停机原因是 **`input-plan: quit`**（配方自己走完），⛔ 不是 `--time-limit` 打的圆场
//      —— 这条断言把"用时间上限把 exit 0 蒙出来"挡在外面（时间上限也 exit 0）；
//   ④ `[profile-window] begin/end label=boot-title` 都在（`--profile` 的区间读数面接上了）；
//      只断言**存在 + 标签**，⛔ 不断言任何 ms / ns 数字（AGENTS.md §12.6：读数进文档，不进 test）；
//   ⑤ 没有 `⛔` / `派发表里没有的 EA` / `decode-error` 这类响亮错。
//   ⇒ 红了意味着：启动到 TITLE 这条端到端路径在 AOT 臂上断了（停点 / 门 / 完成度 / profiler 窗接线之一）。
//
// ★ 能红的两种改法（都**不动产品代码**）：
//   ① 把 `boot-title.plan` 的门换成 TITLE 的退出路径段 `0x388C..0x38A4`（永不成立）⇒ exit 5 ⇒ 红；
//   ② 把 `profile_begin/end boot-title` 删掉 ⇒ ④ 红（窗接线断了）。
//   另：去掉 `--skip-logo` 会让 TITLE 前的帧冻死（`observations.md` 的已知坑）⇒ 也会红。
//
// ★ 为什么在 `LIFT_STRESS` 档：真跑原版安装树（`links/raw`）⇒ 只能进"唯一允许跑真游戏"的那档
//   （`AGENTS.md` §12.4；⛔ 不按"资产在不在"归档）。
// ★ 缺输入（GPU / 字体 / 生成物）⇒ `LIFT_SKIP(77)`（先例 `plugin_e2e`）。
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

}  // namespace

int main(int argc, char** argv) {
    using namespace lift_test;

    if (argc < 5) {
        LIFT_SKIP("需要 CTest 传 <emulator-lift-aot> <执行源> <links/raw> <配方 .plan>（见 CMakeLists）");
    }
    const fs::path aot_exe = argv[1];
    const fs::path age_exe = argv[2];
    const fs::path raw_dir = argv[3];
    const fs::path plan = argv[4];

    std::error_code ec;
    if (!fs::exists(aot_exe, ec)) LIFT_SKIP("emulator-lift-aot 不在场");
    if (!fs::exists(age_exe, ec)) LIFT_SKIP("执行源（corpus/assets/age/AGE.EXE__dumped.sectfix.EXE）不在场");
    if (!fs::exists(raw_dir / "AGE.EXE", ec)) LIFT_SKIP("links/raw/AGE.EXE 不在场（原版安装树）");
    if (!fs::exists(plan, ec)) LIFT_SKIP("配方 scripts/recipes/boot-title.plan 不在场");

    const fs::path root = fs::temp_directory_path(ec) /
                          ("lift-aot-stress-boot-title-" + std::to_string(lift_test::processId()));
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    LIFT_CHECK(!ec);
    const fs::path appdata = root / "appdata";
    fs::create_directories(appdata, ec);

    g_current_case = "aot-stress/boot-title";
    const std::vector<std::string> args = {
        "--exe",        pathToUtf8(age_exe),
        "--game-dir",   pathToUtf8(raw_dir),
        "--appdata",    pathToUtf8(appdata),
        "--present",    "headless",
        "--skip-logo",  "--audio", "off",
        "--time-limit", "90",
        "--profile",
        "--input-plan", pathToUtf8(plan),
    };
    ChildOptions opts;
    opts.cwd = root;
    opts.timeout = std::chrono::milliseconds(180000);
    const ChildResult r = runChild(aot_exe, args, opts);
    LIFT_CHECK(r.started);
    if (!r.started) {
        fs::remove_all(root, ec);
        return finish("aot-stress-boot-title");
    }
    std::printf("aot-stress-boot-title: exit=%d timed_out=%d\n", r.exit_code, r.timed_out ? 1 : 0);

    if (r.err.find("拒绝启动") != std::string::npos)
        LIFT_SKIP("GPU / 窗口初始化不成立（headless 也要 GPU device）");
    if (r.err.find("没有生成物") != std::string::npos) LIFT_SKIP("AOT 生成物不在场（缺 clang / 执行源）");
    if (r.err.find("宿主字体不可用") != std::string::npos) LIFT_SKIP("缺宿主字体（LIFT_HOST_FONT_PATH）");
    if (r.timed_out) {
        failf(__FILE__, __LINE__, "子进程 180 s 未结束（配方没走完也不该挂住）");
        fs::remove_all(root, ec);
        return finish("aot-stress-boot-title");
    }

    if (r.exit_code != 0 || !contains(r.out, "input-plan: quit")) {
        std::fprintf(stderr, "--- stdout (tail) ---\n%s\n--- stderr (tail) ---\n%s\n",
                     r.out.substr(r.out.size() > 4000 ? r.out.size() - 4000 : 0).c_str(),
                     r.err.substr(r.err.size() > 4000 ? r.err.size() - 4000 : 0).c_str());
    }
    // ---- ① 正常结局 ----
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(r.exit_code), 0);
    // ---- ② 完成度判据的正例面：四步全部消费 ----
    LIFT_CHECK(contains(r.out, "input-plan: steps=4"));
    LIFT_CHECK(contains(r.out, "pending=0"));
    // ---- ③ 是配方自己的 `quit` 收的尾（⛔ 不是 `--time-limit` 的圆场）----
    LIFT_CHECK(contains(r.out, "— input-plan: quit"));
    LIFT_CHECK(contains(r.out, "stop      : stopped"));
    // ---- ④ `--profile` 的区间读数面：窗存在、标签对（⛔ 不判 ms）----
    LIFT_CHECK(contains(r.out, "[profile-window] begin label=boot-title"));
    LIFT_CHECK(contains(r.out, "[profile-window] end label=boot-title"));
    LIFT_CHECK(contains(r.out, "=== profile window: boot-title ==="));
    // ---- ⑤ 没有响亮错 ----
    // ⚠ 判据是 `emulator-lift-aot: ⛔` 这个**前缀**（`aot_main` 响亮错的唯一拼法），⛔ 不是裸 `⛔`：
    //    正常读数里 `⛔` 也在**解释性文案**里出现（如 `lift_dyn` 行的 `⛔ 不进闭式`）⇒ 裸判会假红。
    LIFT_CHECK(!contains(r.err, "emulator-lift-aot: ⛔"));
    LIFT_CHECK(!contains(r.err, "派发表里没有的 EA"));
    LIFT_CHECK(!contains(r.err, "decode-error"));
    LIFT_CHECK(!contains(r.err, "input-plan: FAIL"));

    fs::remove_all(root, ec);
    return finish("aot-stress-boot-title");  // 与 CTest 注册名一致
}
