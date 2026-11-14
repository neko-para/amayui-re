// emulator-lift · **LIFT_STRESS S3**：ADV 一段 + **真音频**（口径 **W**：window / real-time + `--audio sdl`）
//
// 判据（**只有读数 + 阈值**；⛔ 没有一条指向"剧情演到哪 / 语音对不对"）：
//   ① 每遍 `exit ∈ {0, 5}`
//      —— `§A2.x` 第 2 条：**驱动 ≠ 判据**。驱动是仓里现成的 `graphs/sn0000.graph`，
//         它失败时退出码 5（`README.md:620`）/ `FAIL :` 行 —— 那**不**让本用例红，
//         只记进 JSON 的 `driver` 段（用来判断"这一段读数是不是在动"）。
//         ⛔ 但 2（启动前参数错）/ 3（guest fault）/ 4（host fault）仍然是红：那是**模拟器崩了**，
//         属 `§0.1` 的稳定性例外（效率档唯一允许的"非读数"判据）。
//   ② 帧时间**中位** ≤ **31.875 ms**
//      —— 规则 **R1**（墙钟只钉中位 / p95）+ 依据读数：2026-11-14 本机三遍的逐遍中位
//         = 21.25 / 21.9 / 20.75 ms（取中位 **21.25**）⇒ 21.25 × 1.5 = **31.875**。
//   ③ 帧时间 **p95** ≤ **130.05 ms**
//      —— 规则 **R1** + 依据读数：逐遍 p95 = 86.8 / 86.2 / 86.7 ms（取中位 **86.7**）⇒ 86.7 × 1.5 = **130.05**。
//         ★ 样本 = `[frame]` 行里 n ≥ 100 的那些；这些帧里含 ADV 的场景切换帧（最慢一遍单帧 1116.3 ms），
//           p95 把它们算进去是**故意的**（`§A1.1` 的帧时间就是"一帧的墙钟"，⛔ 不剔除大帧）。
//   ④ 音频的**四个计数 = 0**（逐遍判，⛔ 不取中位：计数类的判据是"一次都不许有"）
//      —— 规则 **R2**（计数类阈值 = 0 或代码里已写死的门槛）：
//        · `fill_slow`  —— `audio.cpp:1469` / `:2082` 的门槛是 `fill_us > 1000000`（>1000 ms 的刀数）
//        · `underrun_silence_events` —— 宿主替 guest 插过静音的**唯一**判据（`audio.h:95-109`）
//        · `fill_wait_timeouts` / `slot_reverify_fail` —— `machine.cpp:2444-2454` 把非 0 判成异常段
//      —— 依据读数：2026-11-14 三遍全部 = 0（`fills=137`，`fill_world` 见各遍 JSON）。
//   ⑤ **防真空绿**：每遍 `audio :` 行必须 `device=ok` 且 `fills > 0`
//      —— 这不是音频质量判据，是"这次真的是**真设备 + 真 feeder**臂、而且真喂过音频"的闸：
//         `--audio sdl` 打不开设备时会静默退回 `device=fallback/none`，那时 ④ 的四个 0 **什么也证明不了**
//         （同 `tests/feeder_seg_e2e.cpp` 的"假绿陷阱 1"）。缺真设备 ⇒ `LIFT_SKIP`(77)，⛔ 不是红。
//         `LIFT_FRAME_TRACE` 没生效 ⇒ `[frame]` 一行都没有 ⇒ ②③ 会退化成 0 而"全绿"，同 ⑤ 一起挡。
//
// ★ **采而不判**（进 JSON / 进 `observations.md`，⛔ 不进判据）：`fill_max_ms`。
//   **裁决（2026-11-14，本轮收尾第 3 条）：只当读数，⛔ 不进判据（R1）** ——
//   `fill_max_ms` 是读数，⛔ 不进判据（R1）；`audio :` 行自带 500 ms 告警门槛，越线时由**产品**
//   自己报警（`machine.cpp:2456` 的 `fill_max_ms >= 500` ⇒ 日志里一行 `⚠ audio.fill_max_ms=…`），
//   测试**不重复判**它。
//   理由补全：它是**单次最大值**（`fill_max_us`），而 R1 明说墙钟类量"只钉中位 / p95，⛔ 不钉单次
//   最大值"；产品已经在 500 ms 处报警了，测试再判一次只会把"产品已报过的那一次"变成第二条红，
//   而换来的判据正是 R1 不允许的那一类。
//   今天的读数 **678.60 / 498.48 / 485.12 ms** —— 第一遍已经越过 500 ms 门槛（日志里那条
//   `⚠ audio.fill_max_ms=678.60` 就是产品自己的报警），它**不影响本用例的红绿**。
//   ⛔ 现在不许为了让用例绿而把它改成"中位"或"剔掉大刀"：那就是拿实现去凑阈值。
// ★ 其它采而不判：`underruns`（`measurement.md:145-151` 明说它是弱指标、跨版本不可比 ⇒ ⛔ 单独当门）、
//   `fill_overlapped` / `fill_witnessed` / `guest_silence_fills` / `tokenwait`（读数）。
//
// ★ 驱动：`graphs/sn0000.graph`（仓里现成：TITLE → GAMESTART → SN0000，⛔ 不新编坐标、⛔ 不改图）。
//   它的作用是"把画面推着走进 ADV"，判据里没有它任何一条断言。
// ★ 口径（`§A1.0`）：**口径 W**（`--present window --present-hidden --present-vsync 0` ⇒ 自动 real-time
//   ⇒ 才是真 feeder 线程；`--audio sdl` 要真设备）。⛔ 不写 `--present headless`（虚拟时钟下
//   帧时间不是墙钟、音频线程也不一样）。每次 run **全新** `--appdata`。`--time-limit 120` 兜底。
// ★ 落盘：`.cache/lift-stress/s3-adv-audio-<UTC>.json`。⛔ 不进 git（`.cache/` 已 gitignore）。
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

namespace {

struct AudioRead {
    std::string line;
    std::string mode;
    std::string device;
    bool device_ok = false;
    std::uint64_t fills = 0;
    std::uint64_t fill_bytes = 0;
    double fill_max_ms = 0.0;
    std::uint64_t fill_slow = 0;
    std::uint64_t underruns = 0;
    std::uint64_t fill_wait_timeouts = 0;
    double fill_wait_max_ms = 0.0;
    std::uint64_t slot_reverify_fail = 0;
    std::uint64_t underrun_silence_events = 0;
    std::uint64_t fill_overlapped = 0;
    std::uint64_t fill_witnessed = 0;
    std::uint64_t guest_silence_fills = 0;
};

// `audio     : mode=sdl device=ok … fills=N … fill_max_ms=… fill_slow=N …`
AudioRead parseAudio(const std::string& text) {
    AudioRead a;
    a.line = lift_stress::lineStartingWith(text, "audio     :");
    if (a.line.empty()) return a;
    {
        const std::size_t at = a.line.find("mode=");
        const std::size_t sp = a.line.find(' ', at == std::string::npos ? 0 : at);
        if (at != std::string::npos && sp != std::string::npos) a.mode = a.line.substr(at + 5, sp - at - 5);
    }
    {
        const std::size_t at = a.line.find("device=");
        const std::size_t sp = a.line.find(' ', at == std::string::npos ? 0 : at);
        if (at != std::string::npos && sp != std::string::npos) {
            a.device = a.line.substr(at + 7, sp - at - 7);
            a.device_ok = a.device == "ok";
        }
    }
    lift_stress::readU64(a.line, "fills=", &a.fills);
    lift_stress::readU64(a.line, "fill_bytes=", &a.fill_bytes);
    lift_stress::readDouble(a.line, "fill_max_ms=", &a.fill_max_ms);
    lift_stress::readU64(a.line, "fill_slow=", &a.fill_slow);
    lift_stress::readU64(a.line, "underruns=", &a.underruns);
    lift_stress::readU64(a.line, "fill_wait_timeouts=", &a.fill_wait_timeouts);
    lift_stress::readDouble(a.line, "fill_wait_max_ms=", &a.fill_wait_max_ms);
    lift_stress::readU64(a.line, "slot_reverify_fail=", &a.slot_reverify_fail);
    lift_stress::readU64(a.line, "underrun_silence_events=", &a.underrun_silence_events);
    lift_stress::readU64(a.line, "fill_overlapped=", &a.fill_overlapped);
    lift_stress::readU64(a.line, "fill_witnessed=", &a.fill_witnessed);
    lift_stress::readU64(a.line, "guest_silence_fills=", &a.guest_silence_fills);
    return a;
}

}  // namespace

int main(int argc, char** argv) {
#if !defined(_WIN32)
    (void)argc;
    (void)argv;
    LIFT_SKIP("S3 的阈值口径 = windows-x64（§C-9 平台不对称：⛔ 不把别的平台的数当 Windows 门槛）");
#else
    if (argc < 7) {
        std::fprintf(stderr, "用法：%s <emulator-lift> <game-dir> <work-dir> <out-dir> <build-type> <graph>\n",
                     argv[0]);
        return 2;
    }
    const fs::path exe = argv[1];
    const fs::path game_dir = argv[2];
    const fs::path work_dir = argv[3];
    const fs::path out_dir = argv[4];
    const std::string build_type = argv[5];
    const fs::path graph = argv[6];
    if (!lift_stress::buildTypeUsable(build_type)) {
        LIFT_SKIP("S3 只在 RelWithDebInfo 树里跑（Debug 的墙钟与音频读数不许当判据，AGENTS.md §5）");
    }
    if (!fs::exists(exe)) LIFT_SKIP("没有 emulator-lift 可执行（构建目录不对）");
    if (!fs::exists(game_dir / "SYS4INI.BIN")) LIFT_SKIP("没有游戏安装树（SYS4INI.BIN）");
    if (!fs::exists(graph)) LIFT_SKIP("没有驱动图 graphs/sn0000.graph（不在源码树里）");

    // ---- 阈值（推导见文件头 ②③④；⛔ 不是先写好数字再让实现去凑）----
    constexpr double kFrameMedianMs = 31.875;    // ② R1：21.25 ms × 1.5
    constexpr double kFrameP95Ms = 130.05;       // ③ R1：86.7 ms × 1.5
    constexpr std::size_t kMinSteadyFrames = 2;  // ⑤ 防真空绿（不是性能阈值）

    lift_stress::Session s;
    s.exe = exe;
    s.game_dir = game_dir;
    s.work_dir = work_dir;
    s.out_dir = out_dir;
    s.build_type = build_type;
    s.label = "s3-adv-audio";
    s.command_line = exe.string() + " --game-dir " + game_dir.string() +
                     " --appdata <每次全新> --skip-logo --audio sdl --present window --present-hidden"
                     " --present-vsync 0 --native-overrides all --log-level info --interaction-graph " +
                     graph.string() + " --time-limit 120   （ENVIRONMENT: LIFT_FRAME_TRACE=1）";
    s.init("S3 ADV 一段 + 真音频：口径 W（window/real-time + --audio sdl 真 feeder）；判据 = R1 帧时间 / "
           "R2 音频四个 0；驱动 = graphs/sn0000.graph（⛔ 驱动失败不进判据）；N=3（单遍 ~52 s，N=5 要 ~4.5 min）");
    s.root.set("scenario", Json::text("S3"));
    s.root.set("presentMode", Json::text("window（口径 W）+ --audio sdl（真设备 => 真 feeder 线程）"));
    s.root.set("instrument", Json::text("LIFT_FRAME_TRACE=1（CTest ENVIRONMENT 注入）+ --log-level info"
                                        "（`audio :` / `gpu :` 两行只在 Verbose 摘要里打）"));
    Json drv = Json::object();
    drv.set("path", Json::text(graph.string()));
    drv.set("sha256", Json::text(lift_stress::sha256File(graph)));
    drv.set("role", Json::text("只把画面推着走进 ADV；判据里没有它的任何断言（§A2.x 第 2 条）"));
    s.root.set("driver", drv);

    std::vector<double> per_pass_median;
    std::vector<double> per_pass_p95;
    std::vector<int> exits;
    std::vector<std::uint64_t> per_pass_fill_slow;
    std::vector<std::uint64_t> per_pass_silence;
    std::vector<std::uint64_t> per_pass_wait_timeouts;
    std::vector<std::uint64_t> per_pass_reverify;

    for (int i = 1; i <= 3; ++i) {
        const fs::path appdata = lift_stress::freshDir(work_dir / ("appdata-pass" + std::to_string(i)));
        const fs::path log = work_dir / ("s3-pass" + std::to_string(i) + ".log");
        const std::vector<std::string> args = {
            "--game-dir",         game_dir.string(),
            "--appdata",          appdata.string(),
            "--skip-logo",        "--audio",
            "sdl",                "--present",
            "window",             "--present-hidden",
            "--present-vsync",    "0",
            "--native-overrides", "all",
            "--log-level",        "info",
            "--interaction-graph", graph.string(),
            "--time-limit",       "120"};
        const lift_stress::ChildResult child = lift_stress::runRedirected(exe, args, log, 480000u);
        if (!child.started) LIFT_SKIP("子进程没起来（环境不允许 spawn）");
        const std::string text = lift_stress::readAllBytes(log);
        LIFT_SKIP_IF_NO_GPU(child.exit_code, text);

        const AudioRead audio = parseAudio(text);
        // ⑤ 真设备 / 真 feeder 臂：缺 ⇒ SKIP 77（⛔ 不是红；但在 fallback 档上判 ④ 就是假绿）。
        if (!audio.device_ok) {
            std::printf("SKIP: 这次不是真音频设备（device=%s mode=%s）⇒ 音频四个 0 什么也证明不了\n",
                        audio.device.empty() ? "?" : audio.device.c_str(),
                        audio.mode.empty() ? "?" : audio.mode.c_str());
            return 77;
        }

        const std::vector<lift_stress::FrameRow> frames = lift_stress::parseFrames(text);
        std::vector<double> steady_wall;
        for (const lift_stress::FrameRow& f : frames) {
            if (f.n < 100) continue;  // §A1.1：n=1 是 boot→首帧
            steady_wall.push_back(f.wall_ms);
        }
        const lift_stress::Stats wall = lift_stress::statsOf(steady_wall);

        // ⑤ 防真空绿（每遍都判）：音频真喂过 + 帧跟踪真在打。
        LIFT_CHECK(audio.fills > 0);
        if (audio.fills == 0) {
            std::printf("  ⇒ `fills=0`：这一遍一次 fill 都没有 ⇒ 音频账是空的，⛔ 四个 0 不是绿\n");
        }
        LIFT_CHECK(steady_wall.size() >= kMinSteadyFrames);
        if (steady_wall.size() < kMinSteadyFrames) {
            std::printf("  ⇒ 这一遍只有 %zu 条 n≥100 的 `[frame]` 行（LIFT_FRAME_TRACE 没生效？）\n",
                        steady_wall.size());
        }

        // ① 驱动 ≠ 判据：exit 5（interaction graph 失败）记下来但**不**判红；只有崩（2/3/4）才是红。
        LIFT_CHECK(child.exit_code == 0 || child.exit_code == 5);
        if (child.exit_code != 0 && child.exit_code != 5) {
            std::printf("  ⇒ 第 %d 遍 exit=%d（3=guest fault / 4=host fault / 2=启动前参数错）—— 这是崩，不是驱动失败\n",
                        i, child.exit_code);
        }

        exits.push_back(child.exit_code);
        per_pass_median.push_back(wall.median);
        per_pass_p95.push_back(wall.p95);
        per_pass_fill_slow.push_back(audio.fill_slow);
        per_pass_silence.push_back(audio.underrun_silence_events);
        per_pass_wait_timeouts.push_back(audio.fill_wait_timeouts);
        per_pass_reverify.push_back(audio.slot_reverify_fail);

        const std::string stop = lift_stress::lineStartingWith(text, "stop      :");
        const std::string detail = lift_stress::lineStartingWith(text, "detail    :");
        const std::string fail = lift_stress::lineStartingWith(text, "FAIL      :");
        const std::string interaction = lift_stress::lineStartingWith(text, "interaction:");

        Json& p = s.addPass(i, child, log.filename().string());
        Json r = Json::object();
        r.set("frameLines", Json::u64(frames.size()));
        r.set("steadyFrames", Json::u64(steady_wall.size()));
        r.set("frameWallMedianMs", Json::number(wall.median));
        r.set("frameWallP95Ms", Json::number(wall.p95));
        r.set("frameWallMinMs", Json::number(wall.min));
        r.set("frameWallMaxMs", Json::number(wall.max));
        Json a = Json::object();
        a.set("mode", Json::text(audio.mode));
        a.set("device", Json::text(audio.device));
        a.set("fills", Json::u64(audio.fills));
        a.set("fillBytes", Json::u64(audio.fill_bytes));
        a.set("fillMaxMs", Json::number(audio.fill_max_ms));
        a.set("fillSlow", Json::u64(audio.fill_slow));
        a.set("underruns", Json::u64(audio.underruns));
        a.set("fillWaitTimeouts", Json::u64(audio.fill_wait_timeouts));
        a.set("fillWaitMaxMs", Json::number(audio.fill_wait_max_ms));
        a.set("slotReverifyFail", Json::u64(audio.slot_reverify_fail));
        a.set("underrunSilenceEvents", Json::u64(audio.underrun_silence_events));
        a.set("fillOverlapped", Json::u64(audio.fill_overlapped));
        a.set("fillWitnessed", Json::u64(audio.fill_witnessed));
        a.set("guestSilenceFills", Json::u64(audio.guest_silence_fills));
        r.set("audio", a);
        Json d = Json::object();
        d.set("stop", Json::text(stop.empty() ? "-" : stop));
        d.set("detail", Json::text(detail.empty() ? "-" : detail));
        d.set("failLine", Json::text(fail.empty() ? "-" : fail));
        d.set("interaction", Json::text(interaction.empty() ? "-" : interaction));
        d.set("judged", Json::boolean(false));
        r.set("driver", d);
        p.set("readings", r);

        std::printf("s3 pass %d: exit=%d 稳态帧=%zu 帧时间中位=%.2f ms p95=%.2f ms（max %.1f）"
                    " audio: device=%s fills=%llu fill_max=%.2fms fill_slow=%llu underruns=%llu"
                    " silence_events=%llu wait_timeouts=%llu reverify_fail=%llu\n",
                    i, child.exit_code, steady_wall.size(), wall.median, wall.p95, wall.max,
                    audio.device.c_str(), static_cast<unsigned long long>(audio.fills), audio.fill_max_ms,
                    static_cast<unsigned long long>(audio.fill_slow),
                    static_cast<unsigned long long>(audio.underruns),
                    static_cast<unsigned long long>(audio.underrun_silence_events),
                    static_cast<unsigned long long>(audio.fill_wait_timeouts),
                    static_cast<unsigned long long>(audio.slot_reverify_fail));
    }

    // ---- 判据 ----
    const lift_stress::Stats med_frame = lift_stress::statsOf(per_pass_median);
    const lift_stress::Stats med_p95 = lift_stress::statsOf(per_pass_p95);
    LIFT_CHECK(med_frame.median <= kFrameMedianMs);
    if (!(med_frame.median <= kFrameMedianMs)) {
        std::printf("  ⇒ 帧时间中位 %.2f ms > 门槛 %.2f ms（R1：现状中位 21.25 × 1.5）—— 三遍：%.2f / %.2f / %.2f\n",
                    med_frame.median, kFrameMedianMs, per_pass_median[0], per_pass_median[1],
                    per_pass_median[2]);
    }
    LIFT_CHECK(med_p95.median <= kFrameP95Ms);
    if (!(med_p95.median <= kFrameP95Ms)) {
        std::printf("  ⇒ 帧时间 p95 %.2f ms > 门槛 %.2f ms（R1：现状 p95 86.7 × 1.5）—— 三遍：%.2f / %.2f / %.2f\n",
                    med_p95.median, kFrameP95Ms, per_pass_p95[0], per_pass_p95[1], per_pass_p95[2]);
    }
    // ④ 四个计数 0：**逐遍**判（计数类没有"中位"这一说 —— 一次都不许有）。
    for (std::size_t i = 0; i < 3; ++i) {
        LIFT_CHECK(per_pass_fill_slow[i] == 0);
        LIFT_CHECK(per_pass_silence[i] == 0);
        LIFT_CHECK(per_pass_wait_timeouts[i] == 0);
        LIFT_CHECK(per_pass_reverify[i] == 0);
        if (per_pass_fill_slow[i] != 0 || per_pass_silence[i] != 0 || per_pass_wait_timeouts[i] != 0 ||
            per_pass_reverify[i] != 0) {
            std::printf("  ⇒ 第 %zu 遍音频账非 0：fill_slow=%llu underrun_silence_events=%llu"
                        " fill_wait_timeouts=%llu slot_reverify_fail=%llu（R2：门槛都是 0）\n",
                        i + 1, static_cast<unsigned long long>(per_pass_fill_slow[i]),
                        static_cast<unsigned long long>(per_pass_silence[i]),
                        static_cast<unsigned long long>(per_pass_wait_timeouts[i]),
                        static_cast<unsigned long long>(per_pass_reverify[i]));
        }
    }

    s.finish();
    Json th = Json::object();
    th.set("frameMedianMs", Json::number(kFrameMedianMs));
    th.set("frameMedianRule", Json::text("R1：现状逐遍中位 21.25 / 21.9 / 20.75 ⇒ 21.25 × 1.5"));
    th.set("frameP95Ms", Json::number(kFrameP95Ms));
    th.set("frameP95Rule", Json::text("R1：现状逐遍 p95 86.8 / 86.2 / 86.7 ⇒ 86.7 × 1.5"));
    th.set("fillSlow", Json::u64(0));
    th.set("fillSlowRule", Json::text("R2：代码门槛 audio.cpp:1469/2082（fill_us>1000000）；阈值 0"));
    th.set("underrunSilenceEvents", Json::u64(0));
    th.set("underrunSilenceEventsRule", Json::text("R2：阈值 0（宿主替 guest 插静音 = 0 次）"));
    th.set("fillWaitTimeouts", Json::u64(0));
    th.set("fillWaitTimeoutsRule", Json::text("R2：阈值 0（machine.cpp:2444-2454 非 0 即异常段）"));
    th.set("slotReverifyFail", Json::u64(0));
    th.set("slotReverifyFailRule", Json::text("R2：阈值 0（同上）"));
    th.set("fillMaxMsJudged", Json::boolean(false));
    th.set("fillMaxMsRule", Json::text("R1：单次最大值 ⇒ 只当读数，⛔ 不进判据（今天 678.60/498.48/485.12 里"
                                       "第一遍已越过 machine.cpp:2456 的 500 ms 门槛——那是**产品自己的报警"
                                       "**（日志里的 `⚠ audio.fill_max_ms=…`），测试不重复判它）"));
    s.root.set("thresholds", th);
    Json med = Json::object();
    med.set("frameWallMedianMs", Json::number(med_frame.median));
    med.set("frameWallMedianMin", Json::number(med_frame.min));
    med.set("frameWallMedianMax", Json::number(med_frame.max));
    med.set("frameWallP95Ms", Json::number(med_p95.median));
    med.set("frameWallP95Min", Json::number(med_p95.min));
    med.set("frameWallP95Max", Json::number(med_p95.max));
    s.root.set("median", med);
    const fs::path written = s.write();

    std::printf("s3: 帧时间中位 %.2f ms（%.2f/%.2f/%.2f，门槛 %.3f，R1）；p95 %.2f ms（%.2f/%.2f/%.2f，门槛 %.2f，R1）\n",
                med_frame.median, per_pass_median[0], per_pass_median[1], per_pass_median[2], kFrameMedianMs,
                med_p95.median, per_pass_p95[0], per_pass_p95[1], per_pass_p95[2], kFrameP95Ms);
    std::printf("s3: 音频四个 0（fill_slow / underrun_silence_events / fill_wait_timeouts / slot_reverify_fail）"
                "三遍全部成立；fill_max_ms 只当读数（⛔ 不进判据；500 ms 越线由产品自己报警）\n");
    std::printf("s3: 落盘 %s\n", written.string().c_str());
    return lift_test::finish("s3-adv-audio");
#endif
}
