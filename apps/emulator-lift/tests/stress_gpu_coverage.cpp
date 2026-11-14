// emulator-lift · **LIFT_STRESS S9**：GPU 覆盖读数 —— 跑 600 帧，按原因报 `skipped_draws`（口径 **H**）
//
// ★ 为什么这条是四场景里**最重要**的一条：GPU-only 之后（`REQ-01M78BF1FNNH6K8N8GEY6GQYXX`，
//   CPU 光栅已退休）**每一个准入拒批点都是一处潜在缺画** —— 没有 CPU 补救路径了。
//   ⇒ 判据必须是**计数类，阈值 0**（规则 **R2**）：`skipped_draws == 0`。
//
// 判据（**只有读数 + 阈值**；⛔ 没有一条指向"到了某界面 / 某 marker 出现"——那是业务）：
//   ① 三遍全部 `exit 0`（稳定性例外；`--max-frames 600` 正常收口）。
//   ② `skipped_draws == 0`（**逐遍**判）
//      —— 规则 **R2**（计数类阈值取 0）。
//         依据读数（2026-11-14 本机，口径 H，每次全新 `--appdata`）：
//           · 130 帧：`gpu=draws 7969/tri 358551/skipped 0`、`gpu : … skipped=0`、`sprite: … skipped=0`
//           · 200 帧：`gpu=draws 12239/tri 552888/skipped 0`、`skipped=0`、sprite skipped=0
//           · 600 帧：`gpu=draws 36639/tri 1656958/skipped 0`、`skipped=0`、sprite skipped=0
//           · 三处 `skipped top:` **都不打印**（`gpuSkippedTopLine()` 在 `skipped_reasons` 为空时返回空串）
//         ⇒ 门槛 = **0**（不是"实测 0 × 1.5"：R2 对计数类给的就是 0，⛔ 不给计数类留容差）。
//   ③ `sprite_skipped == 0`（**逐遍**判，同一规则 R2）
//      —— ★ 为什么单独判：`gpuNoteSpriteNotDrawn()` 只 +`sprite_skipped` 与 `skipped_reasons[原因]`，
//         **不**加 `skipped_draws` ⇒ 只判 ② 会漏掉"精灵被拒批"这一类。两者都属于"准入拒批 = 潜在缺画"。
//   ④ `skipped top:` 一行都**不许出现**（**逐遍**判，同一规则 R2）
//      —— 它是按原因的计数（`[N×] 原因 ;`，最多 5 条）。②③ 判的是总数，④ 判的是"原因表为空"；
//         三者是同一条不变量的三个面（总数 / 精灵 / 原因表），任一面非 0 都是红。
//   ⑤ `⚠ gpu` 行**不许出现**（= `skipped_draws == 0` **且** `gpu_failures == 0`）
//      —— `machine.cpp:2465` 的条件是 `skipped_draws != 0 || gpu_failures != 0`；
//         后端失败（`gpu_failures`）在摘要里**只有这一行**能看见（`read :` 里没有这一栏）
//         ⇒ "这一行不出现"就是 `gpu_failures == 0` 的读法（R2：阈值 0）。
//   ⑥ **防真空绿**：每遍 `gpu=draws` 必须 > 0、且 `present-tail` 的 `last_frame` 必须 ≥ 600
//      —— 这不是性能判据，是"这次真的跑满了 600 帧、而且 GPU 真的画了东西"的闸：
//         `--max-frames 600` 若因 `--time-limit` 提前收口，② 的 0 只覆盖了前几百帧；
//         而 `draws == 0` 时"没有拒批"是因为**根本没有绘制**（同样的假绿形状）。
//         §A1.0 的稳定性例外允许这一类"读数在不在"的检查。
//
// ★ **红的时候必须逐条列出原因**（这是本用例的设计要求，不是锦上添花）：
//   一旦 ②③④⑤ 有任意一条不成立，失败信息里原样带出 `⚠ gpu` 行、`skipped top:` 行、
//   `gpu :` 行与 `unsupported :` 行 —— 否则"少画了"在输出里会退化成"一个数字变了"，没人知道少了哪一类。
//   ⇒ 实现已**抽成纯函数** `tests/stress_skip_report.h` 的 `judgeSkipReport()`（本文件只负责把真日志喂给它）：
//     输入 = 子进程 stdout 文本 + 阈值，输出 = 通过/失败 + 失败消息。
//     那条"红路径真的会逐条列原因"的机械证据不再靠跑一次真红（~6 min/次）：默认档的
//     `tests/dispatch_tax.cpp` 里有一条 `stress/skip-report` 用**合成文本**三组断言钉住它
//     （`skipped=0` ⇒ 通过 · `skipped=3` + 三行 `skipped top:` ⇒ 失败且逐条含原文 ·
//      `skipped=1` 但原因表缺失 ⇒ 失败且明确说"原因缺失"）。

// ★ 采而不判：`unsupported :` 位图（bit11 `MIPFILTER` 是**已知并接受的近似**，正常 2D 场景恒置位）、
//   `recorded / queued / empty / fragments`、`sprite draws`、`native-ovr hits`（读数）。
//
// ★ 口径（`§A1.0`）：**口径 H** = `--present headless`（虚拟时钟 ⇒ 计数确定性可复现）。
//   同一条命令再加 `--log-level info` 只为让 `gpu :` / `unsupported :` / `sprite:` 三行**真的出现**
//   （它们只在 Verbose 摘要里打；默认档只有 `read :` 一行）。⛔ 这里**不写**任何窗口参数
//   （headless 拒绝窗口参数，`main.cpp:639`）。每次 run **全新** `--appdata`。
//   `--max-frames 600`（跑到 ADV）；`--time-limit 600` 只是超时兜底（正常 ~155 s 收口）。
// ★ N=3 的理由：计数在虚拟时钟下逐遍相同（R2 的 0 本来就没有噪声），但 `§A1.0` 的口径仍是
//   "N 遍取中位、报 min/max" ⇒ 3 遍（N=5 会把这一条推到 ~13 min；⛔ 不用均值、⛔ 不挑最好一遍）。
// ★ 落盘：`.cache/lift-stress/s9-gpu-coverage-<UTC>.json`。⛔ 不进 git（`.cache/` 已 gitignore）。
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "check.h"
#include "stress_harness.h"
#include "stress_skip_report.h"
#include "subprocess_skip.h"

namespace fs = std::filesystem;
using lift_stress::Json;

namespace {

struct GpuRead {
    // `read      : … gpu=draws N/tri N/skipped N …`
    std::uint64_t read_draws = 0;
    std::uint64_t read_tri = 0;
    std::uint64_t read_skipped = 0;
    bool read_ok = false;
    // `gpu       : draws=… gpu=… recorded=… queued=… empty=… fragments=… skipped=…`
    std::uint64_t draws = 0;
    std::uint64_t gpu_draws = 0;
    std::uint64_t recorded = 0;
    std::uint64_t queued = 0;
    std::uint64_t empty = 0;
    std::uint64_t fragments = 0;
    std::uint64_t skipped = 0;
    bool gpu_line_ok = false;
    // `            sprite: draws=… gpu=… skipped=…`
    std::uint64_t sprite_draws = 0;
    std::uint64_t sprite_gpu = 0;
    std::uint64_t sprite_skipped = 0;
    bool sprite_line_ok = false;
    // 原样保留的行（红的时候要逐条带出来）
    std::string gpu_line;
    std::string unsupported_line;
    std::string sprite_line;
    std::string warn_gpu_line;
    std::vector<std::string> skipped_top_lines;
    // `present-tail: last_frame=…`
    std::uint64_t last_frame = 0;
    bool present_tail_ok = false;
};

// `read :` 行里的 `gpu=draws <N>/tri <N>/skipped <N>`（解析实现与 skip 报表**共用一份**，
// 见 `tests/stress_skip_report.h` 的 `parseReadGpu`）。
GpuRead parseGpu(const std::string& text) {
    GpuRead g;
    const std::string read_line = lift_stress::lineStartingWith(text, "read      :");
    g.read_ok = lift_stress::parseReadGpu(read_line, &g.read_draws, &g.read_tri, &g.read_skipped);
    g.gpu_line = lift_stress::lineStartingWith(text, "gpu       : draws=");
    if (!g.gpu_line.empty()) {
        g.gpu_line_ok = lift_stress::readU64(g.gpu_line, "draws=", &g.draws) &&
                        lift_stress::readU64(g.gpu_line, "gpu=", &g.gpu_draws) &&
                        lift_stress::readU64(g.gpu_line, "recorded=", &g.recorded) &&
                        lift_stress::readU64(g.gpu_line, "queued=", &g.queued) &&
                        lift_stress::readU64(g.gpu_line, "empty=", &g.empty) &&
                        lift_stress::readU64(g.gpu_line, "fragments=", &g.fragments) &&
                        lift_stress::readU64(g.gpu_line, "skipped=", &g.skipped);
    }
    g.unsupported_line = lift_stress::lineStartingWith(text, "unsupported :");
    // `            sprite: draws=… gpu=… skipped=…`（缩进 12 空格；用"含 `sprite: draws=`"来找，
    // ⛔ 不写死缩进数：逐帧行的 `sprite_gpu=` 不含 `sprite:`，不会误命中）。
    for (const std::string& line : lift_stress::linesStartingWith(text, "")) {
        if (line.find("sprite: draws=") != std::string::npos) {
            g.sprite_line = line;
            break;
        }
    }
    if (!g.sprite_line.empty()) {
        g.sprite_line_ok = lift_stress::readU64(g.sprite_line, "draws=", &g.sprite_draws) &&
                           lift_stress::readU64(g.sprite_line, "gpu=", &g.sprite_gpu) &&
                           lift_stress::readU64(g.sprite_line, "skipped=", &g.sprite_skipped);
    }
    g.warn_gpu_line = lift_stress::lineStartingWith(text, "⚠ gpu       :");
    g.skipped_top_lines = lift_stress::linesStartingWith(text, "            skipped top:");
    const std::string tail = lift_stress::lineStartingWith(text, "present-tail:");
    if (!tail.empty()) g.present_tail_ok = lift_stress::readU64(tail, "last_frame=", &g.last_frame);
    return g;
}

// ★ **只在红的时候**调用：把每条原因原样带出来（`⚠ gpu` / `skipped top:` / `gpu :` / `unsupported :`）。
//   ⛔ 它不参与判据 —— 它的唯一职责是"少画了"不许退化成一个变了味的数字。
//   ★ 提取与报错文本全在 `tests/stress_skip_report.h` 的 `judgeSkipReport()` 里（纯函数，可被
//     默认档的 `stress/skip-report` 用合成文本断言）；这里只把**真日志**喂进去并原样打印。
void reportSkipReasons(const std::string& text, const lift_stress::SkipThresholds& th) {
    const lift_stress::SkipVerdict verdict = lift_stress::judgeSkipReport(text, th);
    std::fputs(verdict.message.c_str(), stdout);
    if (!verdict.message.empty() && verdict.message.back() != '\n') std::fputc('\n', stdout);
}

}  // namespace

int main(int argc, char** argv) {
#if !defined(_WIN32)
    (void)argc;
    (void)argv;
    LIFT_SKIP("S9 的阈值口径 = windows-x64（§C-9 平台不对称：⛔ 不把别的平台的数当 Windows 门槛）");
#else
    if (argc < 6) {
        std::fprintf(stderr, "用法：%s <emulator-lift> <game-dir> <work-dir> <out-dir> <build-type>\n", argv[0]);
        return 2;
    }
    const fs::path exe = argv[1];
    const fs::path game_dir = argv[2];
    const fs::path work_dir = argv[3];
    const fs::path out_dir = argv[4];
    const std::string build_type = argv[5];
    if (!lift_stress::buildTypeUsable(build_type)) {
        LIFT_SKIP("S9 只在 RelWithDebInfo 树里跑（Debug 的读数不许当判据，AGENTS.md §5）");
    }
    if (!fs::exists(exe)) LIFT_SKIP("没有 emulator-lift 可执行（构建目录不对）");
    if (!fs::exists(game_dir / "SYS4INI.BIN")) LIFT_SKIP("没有游戏安装树（SYS4INI.BIN）");

    // ---- 阈值（推导见文件头 ②③④⑤；全部来自 R2：计数类 = 0）----
    constexpr std::uint64_t kSkippedDraws = 0;
    constexpr std::uint64_t kSpriteSkipped = 0;
    constexpr std::size_t kSkippedTopLines = 0;
    constexpr std::uint64_t kFramesRequested = 600;
    // 同一组门槛交给纯函数（`tests/stress_skip_report.h`）：红的时候由它把原因逐条列出来。
    lift_stress::SkipThresholds skip_th;
    skip_th.skipped_draws = kSkippedDraws;
    skip_th.sprite_skipped = kSpriteSkipped;
    skip_th.skipped_top_lines = kSkippedTopLines;

    lift_stress::Session s;
    s.exe = exe;
    s.game_dir = game_dir;
    s.work_dir = work_dir;
    s.out_dir = out_dir;
    s.build_type = build_type;
    s.label = "s9-gpu-coverage";
    s.command_line = exe.string() + " --game-dir " + game_dir.string() +
                     " --appdata <每次全新> --skip-logo --audio off --present headless"
                     " --native-overrides all --log-level info --max-frames 600 --time-limit 600";
    s.init("S9 GPU 覆盖：口径 H（headless/虚拟时钟）；跑满 600 帧，判据 = R2 计数 0"
           "（skipped_draws / sprite_skipped / skipped top: / ⚠ gpu 四个面）；N=3（单遍 ~155 s，N=5 会到 ~13 min）");
    s.root.set("scenario", Json::text("S9"));
    s.root.set("presentMode", Json::text("headless（口径 H；不接受窗口参数）"));
    s.root.set("instrument", Json::text("--log-level info：让 `gpu :` / `unsupported :` / `sprite:` 三行真的打印"
                                        "（它们只在 Verbose 摘要里；默认档只有 `read :` 一行）"));
    s.root.set("maxFrames", Json::u64(kFramesRequested));

    std::vector<int> exits;
    std::vector<std::uint64_t> skipped_all;
    std::vector<std::uint64_t> sprite_skipped_all;
    std::vector<std::uint64_t> draws_all;

    for (int i = 1; i <= 3; ++i) {
        const fs::path appdata = lift_stress::freshDir(work_dir / ("appdata-pass" + std::to_string(i)));
        const fs::path log = work_dir / ("s9-pass" + std::to_string(i) + ".log");
        const std::vector<std::string> args = {
            "--game-dir",         game_dir.string(),
            "--appdata",          appdata.string(),
            "--skip-logo",        "--audio",
            "off",                "--present",
            "headless",           "--native-overrides",
            "all",                "--log-level",
            "info",               "--max-frames",
            std::to_string(kFramesRequested),
            "--time-limit",       "600"};
        const lift_stress::ChildResult child = lift_stress::runRedirected(exe, args, log, 900000u);
        if (!child.started) LIFT_SKIP("子进程没起来（环境不允许 spawn）");
        const std::string text = lift_stress::readAllBytes(log);
        LIFT_SKIP_IF_NO_GPU(child.exit_code, text);

        const GpuRead g = parseGpu(text);

        // ⑥ 防真空绿（每遍都判）：三行读点都在、且真的画了东西、且真的跑满 600 帧。
        LIFT_CHECK(g.read_ok);
        LIFT_CHECK(g.gpu_line_ok);
        LIFT_CHECK(g.sprite_line_ok);
        LIFT_CHECK(g.present_tail_ok);
        if (!g.read_ok || !g.gpu_line_ok) {
            std::printf("  ⇒ 读点缺失：`read :` 的 `gpu=draws` 或 `gpu       :` 行没解析出来（日志格式变了？）\n");
        }
        LIFT_CHECK(g.read_draws > 0);
        if (g.read_draws == 0) {
            std::printf("  ⇒ `gpu=draws 0`：这一遍**根本没绘制** ⇒ `skipped 0` 什么也证明不了（假绿形状）\n");
        }
        LIFT_CHECK(g.last_frame >= kFramesRequested);
        if (g.last_frame < kFramesRequested) {
            std::printf("  ⇒ 只跑到 frame %llu（要 %llu）：`skipped 0` 覆盖的帧数不够，⛔ 别把它当 600 帧的读数\n",
                        static_cast<unsigned long long>(g.last_frame),
                        static_cast<unsigned long long>(kFramesRequested));
        }

        // ①②③④⑤ 判据（逐遍）。
        LIFT_CHECK(child.exit_code == 0);
        if (child.exit_code != 0) {
            std::printf("  ⇒ 第 %d 遍 exit=%d（看 output-on-failure 里那份日志的 `stop` / `detail`）\n", i,
                        child.exit_code);
        }
        LIFT_CHECK(g.read_skipped == kSkippedDraws);
        LIFT_CHECK(g.skipped == kSkippedDraws);
        LIFT_CHECK(g.sprite_skipped == kSpriteSkipped);
        LIFT_CHECK(g.skipped_top_lines.size() == kSkippedTopLines);
        LIFT_CHECK(g.warn_gpu_line.empty());
        if (g.read_skipped != kSkippedDraws || g.skipped != kSkippedDraws ||
            g.sprite_skipped != kSpriteSkipped || g.skipped_top_lines.size() != kSkippedTopLines ||
            !g.warn_gpu_line.empty()) {
            reportSkipReasons(text, skip_th);
        }

        exits.push_back(child.exit_code);
        skipped_all.push_back(g.skipped);
        sprite_skipped_all.push_back(g.sprite_skipped);
        draws_all.push_back(g.read_draws);

        const std::string stop = lift_stress::lineStartingWith(text, "stop      :");
        const std::string detail = lift_stress::lineStartingWith(text, "detail    :");
        Json& p = s.addPass(i, child, log.filename().string());
        Json r = Json::object();
        r.set("stop", Json::text(stop.empty() ? "-" : stop));
        r.set("detail", Json::text(detail.empty() ? "-" : detail));
        r.set("lastFrame", Json::u64(g.last_frame));
        Json gpu = Json::object();
        gpu.set("readDraws", Json::u64(g.read_draws));
        gpu.set("readTri", Json::u64(g.read_tri));
        gpu.set("readSkipped", Json::u64(g.read_skipped));
        gpu.set("draws", Json::u64(g.draws));
        gpu.set("gpuDraws", Json::u64(g.gpu_draws));
        gpu.set("recorded", Json::u64(g.recorded));
        gpu.set("queued", Json::u64(g.queued));
        gpu.set("empty", Json::u64(g.empty));
        gpu.set("fragments", Json::u64(g.fragments));
        gpu.set("skipped", Json::u64(g.skipped));
        gpu.set("spriteDraws", Json::u64(g.sprite_draws));
        gpu.set("spriteGpu", Json::u64(g.sprite_gpu));
        gpu.set("spriteSkipped", Json::u64(g.sprite_skipped));
        gpu.set("skippedTopLines", Json::u64(g.skipped_top_lines.size()));
        gpu.set("skippedTopVerbatim", Json::text(g.skipped_top_lines.empty() ? "" : g.skipped_top_lines.front()));
        gpu.set("unsupportedVerbatim",
                Json::text(g.unsupported_line.empty() ? "" : g.unsupported_line));
        gpu.set("warnGpuVerbatim", Json::text(g.warn_gpu_line.empty() ? "" : g.warn_gpu_line));
        r.set("gpu", gpu);
        p.set("readings", r);

        std::printf("s9 pass %d: exit=%d last_frame=%llu draws=%llu tri=%llu skipped=%llu"
                    " sprite: draws=%llu skipped=%llu | skipped top: %s\n",
                    i, child.exit_code, static_cast<unsigned long long>(g.last_frame),
                    static_cast<unsigned long long>(g.read_draws),
                    static_cast<unsigned long long>(g.read_tri),
                    static_cast<unsigned long long>(g.read_skipped),
                    static_cast<unsigned long long>(g.sprite_draws),
                    static_cast<unsigned long long>(g.sprite_skipped),
                    g.skipped_top_lines.empty() ? "（不打印 = 原因表为空）" : g.skipped_top_lines.front().c_str());
    }

    s.finish();
    Json th = Json::object();
    th.set("skippedDraws", Json::u64(kSkippedDraws));
    th.set("skippedDrawsRule", Json::text("R2：计数类阈值 0（依据读数：130/200/600 帧三处 skipped=0，"
                                          "`skipped top:` 不打印）"));
    th.set("spriteSkipped", Json::u64(kSpriteSkipped));
    th.set("spriteSkippedRule", Json::text("R2：阈值 0（gpuNoteSpriteNotDrawn 不 +skipped_draws ⇒ 必须单独判）"));
    th.set("skippedTopLines", Json::u64(kSkippedTopLines));
    th.set("skippedTopRule", Json::text("R2：按原因表为空（同一不变量的第三面）"));
    th.set("gpuFailures", Json::u64(0));
    th.set("gpuFailuresRule", Json::text("R2：阈值 0；读法 = `⚠ gpu` 行不出现（machine.cpp:2465 的条件）"));
    th.set("framesRequested", Json::u64(kFramesRequested));
    th.set("framesRequestedRule", Json::text("防真空绿：`present-tail` 的 last_frame 必须 ≥ 600（不是性能阈值）"));
    s.root.set("thresholds", th);
    Json med = Json::object();
    med.set("skippedDraws", Json::u64(lift_stress::medianU64(skipped_all)));
    med.set("spriteSkipped", Json::u64(lift_stress::medianU64(sprite_skipped_all)));
    med.set("draws", Json::u64(lift_stress::medianU64(draws_all)));
    med.set("drawsMin", Json::u64(*std::min_element(draws_all.begin(), draws_all.end())));
    med.set("drawsMax", Json::u64(*std::max_element(draws_all.begin(), draws_all.end())));
    s.root.set("median", med);
    const fs::path written = s.write();

    std::printf("s9: 600 帧 ×3 遍 ⇒ skipped_draws = %llu / %llu / %llu（门槛 %llu，R2）；"
                "sprite_skipped = %llu / %llu / %llu；draws 中位 %llu（%llu–%llu）\n",
                static_cast<unsigned long long>(skipped_all[0]),
                static_cast<unsigned long long>(skipped_all[1]),
                static_cast<unsigned long long>(skipped_all[2]),
                static_cast<unsigned long long>(kSkippedDraws),
                static_cast<unsigned long long>(sprite_skipped_all[0]),
                static_cast<unsigned long long>(sprite_skipped_all[1]),
                static_cast<unsigned long long>(sprite_skipped_all[2]),
                static_cast<unsigned long long>(lift_stress::medianU64(draws_all)),
                static_cast<unsigned long long>(*std::min_element(draws_all.begin(), draws_all.end())),
                static_cast<unsigned long long>(*std::max_element(draws_all.begin(), draws_all.end())));
    std::printf("s9: 落盘 %s\n", written.string().c_str());
    return lift_test::finish("s9-gpu-coverage");
#endif
}
