// emulator-lift · **LIFT_STRESS S2**：TITLE 稳态帧时间（口径 **W**：window / real-time）
//
// 判据（**只有读数 + 阈值**；⛔ 没有一条指向"到了 TITLE / 画面对不对"）：
//   ① 三遍全部 `exit 0`（稳定性例外，见 `§A2.x` 第 3 条）。
//   ② 帧时间**中位** ≤ **46.95 ms**
//      —— 规则 **R1**（墙钟只钉中位 / p95，⛔ 不钉单次最大值）
//         + 依据读数：2026-11-14 本机三遍的**逐遍中位** = 31.3 / 32.2 / 31.3 ms（取中位 31.3）
//         ⇒ 门槛 = 31.3 × 1.5 = **46.95**（与 `§A1.1` 的 TITLE 回归门同一乘数）。
//         ★ 样本 = `LIFT_FRAME_TRACE=1` 的 `[frame]` 行里 **n ≥ 100** 的那些（`§A1.1` 的"稳态"窗口；
//           n=1 是 boot→首帧，会把它混进来 ⇒ 必须切掉）。
//   ③ 帧时间 **p95** ≤ **57.9 ms**
//      —— 规则 **R1** + 依据读数：逐遍 p95 = 38.6 / 39.8 / 38.6 ms（取中位 38.6）⇒ 38.6 × 1.5 = **57.9**。
//   ④ 空转窗口的**指令吞吐** ≤ **178.74 ms/百万条**
//      —— 规则 **R3 的偏离**：现状中位 142.99 ms/M × **1.25**（⛔ 不是 R3 的 ×1.10）
//         + 依据读数：本档标定三遍 = 142.99 / 147.41 / 142.79 ms/M（取中位 142.99）
//         ⇒ 142.99 × 1.25 = 178.7375 ⇒ 取 **178.74**（两位小数，与 46.95 / 57.9 同一写法）。
//         ★ **为什么偏离 R3 的 ×1.10**：R3 的 1.10 派生自**空闲机**上的 1.2–1.4% 遍间噪声
//           （`REQ-01M6XVZC086PFXJRWE5AASRJ8G:59-60`：N=5 中位 105.87、极差 1.2%）；而本档在
//           **共享负载**（宿主忙 18–19%）下观测到的批间 + 遍间离散远大于此：同一命令的第二轮真跑
//           三遍 = **184.37 / 148.71 / 144.91** ms/M —— 单遍 184.37 已经越过旧门 157.29，
//           而 N=3 中位 148.71 离旧门只剩 **5.5%** 余量（`observations.md` OBS-20261114-06）。
//           ⇒ 旧门会把"机器忙"判成回归。×1.25（178.74）仍能抓住**真正的**回归（真实回归通常是
//             2× 量级，不是 5%），同时不会让"机器忙"伪装成回归。
//         ★ ⛔ 不要删掉这条门（那等于回到"没有效率判据"）；也 ⛔ 不要改成"挑最好一遍"
//           —— 判据仍落在 **N=3 的中位**上（R1 精神不变：只钉中位 / p95，⛔ 不钉单遍极值）。
//         ★ 负载档必须**一起读**：JSON 里 `load.cpuBusyPctAtStart/End` 记着这一轮头尾的宿主忙占比
//           采样值；这条门若红，失败消息里也会带出负载档 —— 先看负载，再判断是不是回归。
//         ★ 口径**必须一起读**：这里开着 `LIFT_FRAME_TRACE`（`measurement.md` M-02：开/关不许混表），
//           所以 ⛔ 不要拿 `pnpm tools lift bench idle` 的 105.87 ms/M（**无**帧跟踪）来比。
//         ★ 这个数来自脚本自己的两个打点（`[input-script]` 行的 Δinsns / Δguest_ms；window 模式
//           是 real-time 时钟 ⇒ `guest_ms` 就是宿主毫秒），⛔ 不是 profiler 的估数。
//   ⑤ **防真空绿**：每遍必须真跑出 ≥2 条 n ≥ 100 的 `[frame]` 行
//      —— 这不是性能阈值，是"读数非空"闸：`LIFT_FRAME_TRACE` 没生效时 `[frame]` 一行都没有，
//         ②③ 的样本会是空集 ⇒ 中位/p95 全 0 ⇒ **看起来全绿**（正是 `§A2.x` 要防的形状）。
//         §A1.0 的"稳定性例外"允许这一类"读数在不在"的检查。
//
// ★ 驱动 ≠ 判据：驱动是仓里**现成**的 `scripts/bench/idle.txt`（窗口 `[250M, 450M]` 条指令，
//   ⛔ 不新编脚本、⛔ 不改它的窗口）；它的 `quit` 让 run 自己收口，`--time-limit 90` 只是兜底。
// ★ 口径（`§A1.0`）：**口径 W** = `--present window --present-hidden --present-vsync 0`
//   （窗口模式自动 real-time ⇒ `guest_ms` = 宿主毫秒）。⛔ 这里**不写** `--present headless`
//   （headless 是虚拟时钟，帧时间就不是墙钟了；两套参数不许混写，`main.cpp:639`）。
//   每次 run **全新** `--appdata`。RelWithDebInfo / windows-x64（Debug 与其它平台 `LIFT_SKIP` 77）。
// ★ 落盘：`.cache/lift-stress/s2-title-steady-<UTC>.json`（逐遍原始值 + 中位 + exe sha256 +
//   `CMAKE_BUILD_TYPE` + 工作树指纹 + 负载档）。⛔ 不进 git（`.cache/` 已 gitignore）。
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "check.h"
#include "stress_harness.h"
#include "subprocess_skip.h"

namespace fs = std::filesystem;
using lift_stress::Json;

int main(int argc, char** argv) {
#if !defined(_WIN32)
    (void)argc;
    (void)argv;
    LIFT_SKIP("S2 的阈值口径 = windows-x64（§C-9 平台不对称：⛔ 不把别的平台的数当 Windows 门槛）");
#else
    if (argc < 7) {
        std::fprintf(stderr, "用法：%s <emulator-lift> <game-dir> <work-dir> <out-dir> <build-type> <idle.txt>\n",
                     argv[0]);
        return 2;
    }
    const fs::path exe = argv[1];
    const fs::path game_dir = argv[2];
    const fs::path work_dir = argv[3];
    const fs::path out_dir = argv[4];
    const std::string build_type = argv[5];
    const fs::path script = argv[6];
    if (!lift_stress::buildTypeUsable(build_type)) {
        LIFT_SKIP("S2 只在 RelWithDebInfo 树里跑（Debug 的墙钟不许当判据，AGENTS.md §5）");
    }
    if (!fs::exists(exe)) LIFT_SKIP("没有 emulator-lift 可执行（构建目录不对）");
    if (!fs::exists(game_dir / "SYS4INI.BIN")) LIFT_SKIP("没有游戏安装树（SYS4INI.BIN）");
    if (!fs::exists(script)) LIFT_SKIP("没有工况脚本 scripts/bench/idle.txt（不在源码树里）");

    // ---- 三条阈值（推导见文件头 ②③④；⛔ 不是先写好数字再让实现去凑）----
    constexpr double kFrameMedianMs = 46.95;   // ② R1：31.3 ms × 1.5
    constexpr double kFrameP95Ms = 57.9;       // ③ R1：38.6 ms × 1.5
    constexpr double kMsPerMinsn = 178.74;     // ④ R3 偏离：142.99 ms/M × 1.25（为什么不是 ×1.10 见文件头 ④）
    constexpr std::size_t kMinSteadyFrames = 2;  // ⑤ 防真空绿（不是性能阈值）

    lift_stress::Session s;
    s.exe = exe;
    s.game_dir = game_dir;
    s.work_dir = work_dir;
    s.out_dir = out_dir;
    s.build_type = build_type;
    s.label = "s2-title-steady";
    s.command_line = exe.string() + " --game-dir " + game_dir.string() +
                     " --appdata <每次全新> --skip-logo --audio off --present window --present-hidden"
                     " --present-vsync 0 --native-overrides all --input-script " + script.string() +
                     " --time-limit 90   （ENVIRONMENT: LIFT_FRAME_TRACE=1）";
    s.init("S2 TITLE 稳态帧时间：口径 W（window/real-time）+ LIFT_FRAME_TRACE=1；判据 = R1 中位/p95 + "
           "R3 偏离（×1.25）吞吐；N=3（N=5 会把三条场景的总时长推到 ~10 min，本批取 3 遍取中位，⛔ 不用均值）");
    s.root.set("scenario", Json::text("S2"));
    s.root.set("presentMode", Json::text("window（口径 W；--present-hidden + --present-vsync 0）"));
    s.root.set("instrument", Json::text("LIFT_FRAME_TRACE=1（由 CTest 的 ENVIRONMENT 注入；开/关不许混表）"));
    Json sc = Json::object();
    sc.set("path", Json::text(script.string()));
    sc.set("sha256", Json::text(lift_stress::sha256File(script)));
    s.root.set("script", sc);

    std::vector<double> per_pass_median;
    std::vector<double> per_pass_p95;
    std::vector<double> per_pass_ms_per_minsn;
    std::vector<int> exits;

    for (int i = 1; i <= 3; ++i) {
        const fs::path appdata = lift_stress::freshDir(work_dir / ("appdata-pass" + std::to_string(i)));
        const fs::path log = work_dir / ("s2-pass" + std::to_string(i) + ".log");
        const std::vector<std::string> args = {
            "--game-dir",   game_dir.string(), "--appdata",      appdata.string(), "--skip-logo",
            "--audio",      "off",             "--present",      "window",         "--present-hidden",
            "--present-vsync", "0",            "--native-overrides", "all",        "--input-script",
            script.string(), "--time-limit",   "90"};
        const lift_stress::ChildResult child = lift_stress::runRedirected(exe, args, log, 300000u);
        if (!child.started) LIFT_SKIP("子进程没起来（环境不允许 spawn）");
        const std::string text = lift_stress::readAllBytes(log);
        LIFT_SKIP_IF_NO_GPU(child.exit_code, text);

        const std::vector<lift_stress::FrameRow> frames = lift_stress::parseFrames(text);
        std::vector<double> steady_wall;
        std::vector<double> steady_insns;
        for (const lift_stress::FrameRow& f : frames) {
            if (f.n < 100) continue;  // ★ `§A1.1`：n=1 是 boot→首帧，不算稳态
            steady_wall.push_back(f.wall_ms);
            steady_insns.push_back(static_cast<double>(f.insns));
        }
        const lift_stress::Stats wall = lift_stress::statsOf(steady_wall);
        const lift_stress::Stats insns = lift_stress::statsOf(steady_insns);

        // 吞吐：脚本自己的两个打点（去掉同一 tick 的重复点，与 `tools/lift.mjs` 同口径）。
        const std::vector<lift_stress::ScriptMark> marks =
            lift_stress::dedupeMarks(lift_stress::parseScriptMarks(text));
        std::uint64_t window_host_ms = 0;
        std::uint64_t window_insns = 0;
        double ms_per_minsn = 0.0;
        if (marks.size() >= 2) {
            window_host_ms = marks.back().guest_ms - marks.front().guest_ms;
            window_insns = marks.back().insns - marks.front().insns;
            if (window_host_ms > 0 && window_insns > 0) {
                ms_per_minsn = static_cast<double>(window_host_ms) * 1e6 / static_cast<double>(window_insns);
            }
        }

        // ⑤ 防真空绿（每遍都判，不只是第一遍）：读数非空 + 两个打点都在。
        LIFT_CHECK(!lift_stress::lineStartingWith(text, "read      :").empty());
        LIFT_CHECK(steady_wall.size() >= kMinSteadyFrames);
        LIFT_CHECK(marks.size() >= 2);
        LIFT_CHECK(window_host_ms > 0 && window_insns > 0);
        if (steady_wall.size() < kMinSteadyFrames) {
            std::printf("  ⇒ 这一遍只有 %zu 条 n≥100 的 `[frame]` 行（LIFT_FRAME_TRACE 没生效？）"
                        "⇒ 中位/p95 会是空集的 0，⛔ 那不是绿\n",
                        steady_wall.size());
        }

        exits.push_back(child.exit_code);
        per_pass_median.push_back(wall.median);
        per_pass_p95.push_back(wall.p95);
        per_pass_ms_per_minsn.push_back(ms_per_minsn);

        Json& p = s.addPass(i, child, log.filename().string());
        Json r = Json::object();
        r.set("frameLines", Json::u64(frames.size()));
        r.set("steadyFrames", Json::u64(steady_wall.size()));
        r.set("frameWallMedianMs", Json::number(wall.median));
        r.set("frameWallP95Ms", Json::number(wall.p95));
        r.set("frameWallMinMs", Json::number(wall.min));
        r.set("frameWallMaxMs", Json::number(wall.max));
        r.set("frameInsnsMedian", Json::number(insns.median));
        r.set("frameInsnsMin", Json::number(insns.min));
        r.set("frameInsnsMax", Json::number(insns.max));
        r.set("windowGuestMs", Json::u64(window_host_ms));
        r.set("windowInsns", Json::u64(window_insns));
        r.set("msPerMinsn", Json::number(ms_per_minsn));
        r.set("marks", Json::u64(marks.size()));
        p.set("readings", r);

        std::printf("s2 pass %d: exit=%d 稳态帧=%zu 帧时间中位=%.2f ms p95=%.2f ms（min %.2f / max %.2f）"
                    " 每帧 insns 中位=%.0f 空转=%.2f ms/M（窗口 %llu 条 / %llu ms）\n",
                    i, child.exit_code, steady_wall.size(), wall.median, wall.p95, wall.min, wall.max,
                    insns.median, ms_per_minsn, static_cast<unsigned long long>(window_insns),
                    static_cast<unsigned long long>(window_host_ms));
    }

    // ---- 判据（N=3 取中位：判据落在**三遍的中位**上；单遍极值只报不判 —— R1）----
    for (std::size_t i = 0; i < exits.size(); ++i) {
        LIFT_CHECK(exits[i] == 0);
        if (exits[i] != 0) std::printf("  ⇒ 第 %zu 遍 exit=%d（看 output-on-failure 里那份日志）\n", i + 1, exits[i]);
    }
    const lift_stress::Stats med_frame = lift_stress::statsOf(per_pass_median);
    const lift_stress::Stats med_p95 = lift_stress::statsOf(per_pass_p95);
    const lift_stress::Stats med_mmpm = lift_stress::statsOf(per_pass_ms_per_minsn);

    LIFT_CHECK(med_frame.median <= kFrameMedianMs);
    if (!(med_frame.median <= kFrameMedianMs)) {
        std::printf("  ⇒ 帧时间中位 %.2f ms > 门槛 %.2f ms（R1：现状中位 31.3 × 1.5）—— 三遍：%.2f / %.2f / %.2f\n",
                    med_frame.median, kFrameMedianMs, per_pass_median[0], per_pass_median[1],
                    per_pass_median[2]);
    }
    LIFT_CHECK(med_p95.median <= kFrameP95Ms);
    if (!(med_p95.median <= kFrameP95Ms)) {
        std::printf("  ⇒ 帧时间 p95 %.2f ms > 门槛 %.2f ms（R1：现状 p95 38.6 × 1.5）—— 三遍：%.2f / %.2f / %.2f\n",
                    med_p95.median, kFrameP95Ms, per_pass_p95[0], per_pass_p95[1], per_pass_p95[2]);
    }
    LIFT_CHECK(med_mmpm.median <= kMsPerMinsn);
    if (!(med_mmpm.median <= kMsPerMinsn)) {
        std::printf("  ⇒ 空转吞吐 %.2f ms/M > 门槛 %.2f ms/M（R3 偏离：现状中位 142.99 × 1.25）"
                    "—— 三遍：%.2f / %.2f / %.2f\n",
                    med_mmpm.median, kMsPerMinsn, per_pass_ms_per_minsn[0], per_pass_ms_per_minsn[1],
                    per_pass_ms_per_minsn[2]);
        // ★ 负载档（红了先看这一行）：§0.2 R1 明说"不同负载下的墙钟读数不可横比" ⇒
        //   "机器忙"不是回归。采样 = 宿主 CPU 忙占比（Windows 没有 loadavg，见 `stress_harness.h`）。
        char load_start_text[32] = "采样失败";
        char load_now_text[32] = "采样失败";
        if (s.load_start_ok) std::snprintf(load_start_text, sizeof(load_start_text), "%.2f%%", s.load_start);
        double busy_now = 0.0;
        if (lift_stress::cpuBusyPercent(&busy_now)) {
            std::snprintf(load_now_text, sizeof(load_now_text), "%.2f%%", busy_now);
        }
        std::printf("  ⇒ 负载档（宿主 CPU 忙占比）：起始 %s → 现在 %s；×1.25 的余量正是按"
                    "**共享负载**（18–19%%，OBS-20261114-06）选的\n",
                    load_start_text, load_now_text);
    }

    s.finish();
    Json th = Json::object();
    th.set("frameMedianMs", Json::number(kFrameMedianMs));
    th.set("frameMedianRule", Json::text("R1：现状逐遍中位 31.3 / 32.2 / 31.3 ⇒ 31.3 × 1.5"));
    th.set("frameP95Ms", Json::number(kFrameP95Ms));
    th.set("frameP95Rule", Json::text("R1：现状逐遍 p95 38.6 / 39.8 / 38.6 ⇒ 38.6 × 1.5"));
    th.set("msPerMinsn", Json::number(kMsPerMinsn));
    th.set("msPerMinsnRule", Json::text("R3 偏离：现状逐遍 142.99 / 147.41 / 142.79 ⇒ 中位 142.99 × 1.25"
                                        "（⛔ 不是 R3 的 ×1.10：那个乘数来自空闲机 1.2–1.4% 噪声；本档在"
                                        "共享负载 18–19% 下遍间离散远大于此，见文件头 ④ 与 OBS-20261114-06）"));
    th.set("minSteadyFrames", Json::u64(kMinSteadyFrames));
    th.set("minSteadyFramesRule", Json::text("防真空绿：读数非空（不是性能阈值）"));
    s.root.set("thresholds", th);
    Json med = Json::object();
    med.set("frameWallMedianMs", Json::number(med_frame.median));
    med.set("frameWallMedianMin", Json::number(med_frame.min));
    med.set("frameWallMedianMax", Json::number(med_frame.max));
    med.set("frameWallP95Ms", Json::number(med_p95.median));
    med.set("frameWallP95Min", Json::number(med_p95.min));
    med.set("frameWallP95Max", Json::number(med_p95.max));
    med.set("msPerMinsn", Json::number(med_mmpm.median));
    med.set("msPerMinsnMin", Json::number(med_mmpm.min));
    med.set("msPerMinsnMax", Json::number(med_mmpm.max));
    s.root.set("median", med);
    const fs::path written = s.write();

    std::printf("s2: 帧时间中位 %.2f ms（%.2f/%.2f/%.2f，门槛 %.2f = 现状×1.5，R1）\n", med_frame.median,
                per_pass_median[0], per_pass_median[1], per_pass_median[2], kFrameMedianMs);
    std::printf("s2: p95 %.2f ms（%.2f/%.2f/%.2f，门槛 %.2f，R1）；吞吐 %.2f ms/M（%.2f/%.2f/%.2f，"
                "门槛 %.2f，R3 偏离 = ×1.25）\n",
                med_p95.median, per_pass_p95[0], per_pass_p95[1], per_pass_p95[2], kFrameP95Ms,
                med_mmpm.median, per_pass_ms_per_minsn[0], per_pass_ms_per_minsn[1],
                per_pass_ms_per_minsn[2], kMsPerMinsn);
    std::printf("s2: 落盘 %s\n", written.string().c_str());
    return lift_test::finish("s2-title-steady");
#endif
}
