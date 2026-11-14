// emulator-lift · **LIFT_STRESS S1**：boot → TITLE 首帧（口径 **H**：headless / 确定性读数）
//
// 判据（**只有读数 + 阈值**；⛔ 没有一条指向"游戏做对了"）——每条都写清"哪条规则 + 哪条读数"：
//   ① 三遍全部 `exit 0`
//      —— 稳定性例外（`§0.1` 表最后一行 / `§A2.x` 第 3 条）：效率档唯一允许的"非读数"判据是"没崩"。
//   ② 宿主墙钟**中位** ≤ **7290 ms**
//      —— 规则 **R1**（墙钟只钉中位 / p95，⛔ 不钉单次最大值）
//         + 依据读数：2026-11-14 本机 `--max-frames 1` 三遍 = **4860 / 5000 / 4831 ms**（中位 4860）
//         ⇒ 门槛 = 4860 × 1.5（R1 的 1.5×，与 `§A1.1` 的 TITLE 回归门同一乘数）= **7290**。
//   ③ 宿主墙钟**单遍最大值** ≤ **30000 ms**
//      —— `§A1.4` 的硬上限（历史读数 12.68 s / 25.35 s / 34.7 s 之上留够"旧口径"余量）；
//         ⛔ 它**不**参与②的派生（R1 明说单次最大值不许当门槛来源，这里只是"别炸到 30 s"的兜底）。
//   ④ 三遍 `insns` 的**极差 = 0**
//      —— 规则 **R2**（计数类阈值取 0）+ `§A1.4` 末行"读数可复现"：同一命令两次 run 的 `insns`
//         必须逐字相同，否则墙钟读数本身不可比（虚拟时钟 / 宿主无随机源）。
//         依据读数：2026-11-14 三遍 = `157,120,091`（逐字相同）。
//   ⑤ `read :` 的 `heap=` in_use ≤ **12,215,184 B**
//      —— 规则 **R4**（内存回归门 = 现状 +1 MiB）+ 依据读数：**11,166,608 B**（三遍逐字相同）
//         ⇒ 11,166,608 + 1,048,576 = 12,215,184。
//         ★ `read :` 里那个字段写作 `heap=<in_use>/<peak>KiB`，但值是**字节**（`heap_.bytesInUse()`）
//           ⇒ 这里按字节比，⛔ 不做单位换算去猜。
//
// ★ 采而不判（进 JSON / 进 `observations.md`，不进判据）：
//   `read :` 的 `arena=` 两栏、`arena :` 行的 `surfaces / in_use / peak / largest-free`、
//   `present-tail`、启动回声里的 `raster :` / `present :` 两行。
//   ⇒ 理由：这三类量的**绝对值门槛**在仓里没有已裁决的数（`§C-5`：逐场景现状读数今天才第一次量），
//     先攒读数、下一批再按 R4 钉。
//
// ★ 口径（`§A1.0`）：`--present headless` = **口径 H**（虚拟时钟、确定性读数）；⛔ 它不接受任何窗口参数
//   （`main.cpp:639`），所以本文件**不写** `--present-vsync` / `--present-hidden`。
//   每次 run 都是**全新** `--appdata`；`--time-limit 120` 只是超时兜底（正常 ~5 s 就收口）。
//   RelWithDebInfo / windows-x64；Debug 与其它平台在 `main` 开头 `LIFT_SKIP`(77)（⛔ 不拿 Debug 墙钟当判据）。
// ★ 子进程输出走**文件重定向**（⛔ 不走管道：受限沙箱里捕获管道会 EPERM）。
// ★ 落盘：`.cache/lift-stress/s1-boot-first-frame-<UTC>.json`（逐遍原始值 + 中位 + exe sha256 +
//   `CMAKE_BUILD_TYPE` + 工作树指纹 + 负载档）。⛔ 不进 git（`.cache/` 已 gitignore）。
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

// `read :` 行里的 `heap=<in_use>/<peak>KiB`（值是**字节**，见文件头⑤）。
// ★ 必须**从 `heap=` 之后**找那个 `/`：同一行前面还有 `rep-bulk=21908/0` / `tokenwait=0/0.0ms`。
bool parseHeap(const std::string& read_line, std::uint64_t* in_use, std::uint64_t* peak) {
    const std::size_t at = read_line.find("heap=");
    if (at == std::string::npos) return false;
    const char* begin = read_line.c_str() + at + 5;
    char* end = nullptr;
    const unsigned long long a = std::strtoull(begin, &end, 10);
    if (end == begin || *end != '/') return false;
    const char* b = end + 1;
    char* end2 = nullptr;
    const unsigned long long p = std::strtoull(b, &end2, 10);
    if (end2 == b) return false;
    *in_use = a;
    *peak = p;
    return true;
}

// `read :` 行里的 `arena=<in_use>/<peak>`。
bool parseArenaReadFinal(const std::string& read_line, std::uint64_t* in_use, std::uint64_t* peak) {
    const std::size_t at = read_line.find("arena=");
    if (at == std::string::npos) return false;
    const char* begin = read_line.c_str() + at + 6;
    char* end = nullptr;
    const unsigned long long a = std::strtoull(begin, &end, 10);
    if (end == begin || *end != '/') return false;
    const char* b = end + 1;
    char* end2 = nullptr;
    const unsigned long long p = std::strtoull(b, &end2, 10);
    if (end2 == b) return false;
    *in_use = a;
    *peak = p;
    return true;
}

// `arena     : surfaces N in_use U peak P largest-free F`
bool parseArenaLine(const std::string& line, std::uint64_t* surfaces, std::uint64_t* in_use,
                    std::uint64_t* peak, std::uint64_t* largest_free) {
    return lift_stress::readU64(line, "surfaces ", surfaces) && lift_stress::readU64(line, "in_use ", in_use) &&
           lift_stress::readU64(line, "peak ", peak) && lift_stress::readU64(line, "largest-free ", largest_free);
}

}  // namespace

int main(int argc, char** argv) {
#if !defined(_WIN32)
    (void)argc;
    (void)argv;
    LIFT_SKIP("S1 的阈值口径 = windows-x64（§C-9 平台不对称：⛔ 不把别的平台的数当 Windows 门槛）");
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
        LIFT_SKIP("S1 只在 RelWithDebInfo 树里跑（Debug 的墙钟不许当判据，AGENTS.md §5）");
    }
    if (!fs::exists(exe)) LIFT_SKIP("没有 emulator-lift 可执行（构建目录不对）");
    if (!fs::exists(game_dir / "SYS4INI.BIN")) LIFT_SKIP("没有游戏安装树（SYS4INI.BIN）");

    // ---- 四条阈值（推导见文件头 ②③④⑤；⛔ 不是先写好数字再让实现去凑）----
    constexpr double kWallMedianMs = 7290.0;                 // ②  R1：4860 ms × 1.5
    constexpr double kWallMaxMs = 30000.0;                   // ③  §A1.4 硬上限（兜底，不参与②）
    constexpr std::uint64_t kInsnsSpread = 0;                // ④  R2：计数类阈值 0（读数可复现）
    constexpr std::uint64_t kHeapInUseMax = 12215184ull;     // ⑤  R4：11,166,608 + 1 MiB

    lift_stress::Session s;
    s.exe = exe;
    s.game_dir = game_dir;
    s.work_dir = work_dir;
    s.out_dir = out_dir;
    s.build_type = build_type;
    s.label = "s1-boot-first-frame";
    s.command_line = exe.string() +
                     " --game-dir " + game_dir.string() + " --appdata <每次全新> --skip-logo --audio off"
                     " --present headless --native-overrides all --max-frames 1 --time-limit 120";
    s.init("S1 boot→TITLE 首帧：口径 H（headless/虚拟时钟）；判据 = R1 中位 + R2 计数 + R4 内存；"
           "N=3（不是 N=5：单遍 ~5 s，但墙钟读数按 §A1.0 取中位即可，⛔ 不用均值）");
    s.root.set("scenario", Json::text("S1"));
    s.root.set("presentMode", Json::text("headless（口径 H；不接受窗口参数）"));

    std::vector<double> walls;
    std::vector<std::uint64_t> insns_all;
    std::vector<std::uint64_t> heap_all;
    std::vector<int> exits;

    for (int i = 1; i <= 3; ++i) {
        const fs::path appdata = lift_stress::freshDir(work_dir / ("appdata-pass" + std::to_string(i)));
        const fs::path log = work_dir / ("s1-pass" + std::to_string(i) + ".log");
        const std::vector<std::string> args = {
            "--game-dir", game_dir.string(), "--appdata", appdata.string(), "--skip-logo", "--audio", "off",
            "--present", "headless", "--native-overrides", "all", "--max-frames", "1", "--time-limit", "120"};
        const lift_stress::ChildResult child = lift_stress::runRedirected(exe, args, log, 300000u);
        if (!child.started) LIFT_SKIP("子进程没起来（环境不允许 spawn）");
        const std::string text = lift_stress::readAllBytes(log);
        LIFT_SKIP_IF_NO_GPU(child.exit_code, text);

        const std::string stop = lift_stress::lineStartingWith(text, "stop      :");
        const std::string detail = lift_stress::lineStartingWith(text, "detail    :");
        const std::string insns_line = lift_stress::lineStartingWith(text, "insns     :");
        const std::string read_line = lift_stress::lineStartingWith(text, "read      :");
        const std::string arena_line = lift_stress::lineStartingWith(text, "arena     :");

        std::uint64_t insns = 0, heap_in_use = 0, heap_peak = 0;
        std::uint64_t arena_in_use = 0, arena_peak = 0;
        std::uint64_t surfaces = 0, arena_line_in_use = 0, arena_line_peak = 0, largest_free = 0;
        const bool got_insns = lift_stress::readU64(insns_line, ": ", &insns);
        const bool got_heap = parseHeap(read_line, &heap_in_use, &heap_peak);
        const bool got_arena_read = parseArenaReadFinal(read_line, &arena_in_use, &arena_peak);
        const bool got_arena_line = parseArenaLine(arena_line, &surfaces, &arena_line_in_use,
                                                   &arena_line_peak, &largest_free);
        if (i == 1) {
            // 读数缺失是**红**（不是 SKIP）：它说明日志格式变了、本用例的读点已经失效。
            LIFT_CHECK(!stop.empty());
            LIFT_CHECK(!read_line.empty());
            LIFT_CHECK(got_insns);
            LIFT_CHECK(got_heap);
            LIFT_CHECK(got_arena_read);
            LIFT_CHECK(got_arena_line);
        }

        walls.push_back(child.wall_ms);
        insns_all.push_back(insns);
        heap_all.push_back(heap_in_use);
        exits.push_back(child.exit_code);

        Json& p = s.addPass(i, child, log.filename().string());
        Json r = Json::object();
        r.set("stop", Json::text(stop.empty() ? "-" : stop));
        r.set("detail", Json::text(detail.empty() ? "-" : detail));
        r.set("insns", Json::u64(insns));
        r.set("heapInUse", Json::u64(heap_in_use));
        r.set("heapPeak", Json::u64(heap_peak));
        r.set("arenaInUse", Json::u64(arena_in_use));
        r.set("arenaPeak", Json::u64(arena_peak));
        r.set("arenaSurfaces", Json::u64(surfaces));
        r.set("arenaLineInUse", Json::u64(arena_line_in_use));
        r.set("arenaLinePeak", Json::u64(arena_line_peak));
        r.set("arenaLargestFree", Json::u64(largest_free));
        r.set("hostWallMs", Json::number(child.wall_ms));
        p.set("readings", r);

        std::printf("s1 pass %d: exit=%d 宿主墙钟=%.1f ms insns=%llu heap_in_use=%llu B arena_in_use=%llu\n", i,
                    child.exit_code, child.wall_ms, static_cast<unsigned long long>(insns),
                    static_cast<unsigned long long>(heap_in_use), static_cast<unsigned long long>(arena_in_use));
    }

    // ---- 判据（顺序 = 失败时最想先看到的那个在前）----
    // ① exit 0（稳定性例外：效率档唯一允许的"非读数"判据是"没崩 / 不是 guest fault"）。
    for (std::size_t i = 0; i < exits.size(); ++i) {
        LIFT_CHECK(exits[i] == 0);
        if (exits[i] != 0) {
            std::printf("  ⇒ 第 %zu 遍 exit=%d（3=guest fault / 4=host fault / 2=启动前参数错；"
                        "读 ctest 的 output-on-failure 里那份日志）\n",
                        i + 1, exits[i]);
        }
    }
    const lift_stress::Stats wall = lift_stress::statsOf(walls);
    std::uint64_t insns_min = insns_all.front(), insns_max = insns_all.front();
    std::uint64_t heap_max = heap_all.front();
    for (const std::uint64_t v : insns_all) {
        insns_min = std::min(insns_min, v);
        insns_max = std::max(insns_max, v);
    }
    for (const std::uint64_t v : heap_all) heap_max = std::max(heap_max, v);

    LIFT_CHECK(wall.median <= kWallMedianMs);
    if (!(wall.median <= kWallMedianMs)) {
        std::printf("  ⇒ 中位 %.1f ms > 门槛 %.1f ms（R1：标定中位 4860 × 1.5）—— 逐遍：%.1f / %.1f / %.1f\n",
                    wall.median, kWallMedianMs, walls[0], walls[1], walls[2]);
    }
    LIFT_CHECK(wall.max <= kWallMaxMs);
    if (!(wall.max <= kWallMaxMs)) {
        std::printf("  ⇒ 最慢一遍 %.1f ms > 硬上限 %.1f ms（§A1.4；⛔ 这不是噪声，是 boot 真的变慢了）\n",
                    wall.max, kWallMaxMs);
    }
    LIFT_CHECK(insns_max - insns_min == kInsnsSpread);
    if (insns_max - insns_min != kInsnsSpread) {
        std::printf("  ⇒ 三遍 insns 不一致（min %llu / max %llu）：读数不可比，⛔ 别拿这三遍的中位当基线\n",
                    static_cast<unsigned long long>(insns_min), static_cast<unsigned long long>(insns_max));
    }
    LIFT_CHECK(heap_max <= kHeapInUseMax);
    if (heap_max > kHeapInUseMax) {
        std::printf("  ⇒ 主堆 in_use 最大 %llu B > 门槛 %llu B（R4：现状 11,166,608 + 1 MiB）\n",
                    static_cast<unsigned long long>(heap_max), static_cast<unsigned long long>(kHeapInUseMax));
    }

    s.finish();
    Json th = Json::object();
    th.set("wallMedianMs", Json::number(kWallMedianMs));
    th.set("wallMedianRule", Json::text("R1：现状中位 4860 ms × 1.5（2026-11-14 本机 N=3 = 4860/5000/4831）"));
    th.set("wallMaxMs", Json::number(kWallMaxMs));
    th.set("wallMaxRule", Json::text("§A1.4 硬上限（兜底；不参与中位派生）"));
    th.set("insnsSpread", Json::u64(kInsnsSpread));
    th.set("insnsSpreadRule", Json::text("R2：计数类阈值 0（读数可复现；三遍 = 157,120,091）"));
    th.set("heapInUseMax", Json::u64(kHeapInUseMax));
    th.set("heapInUseRule", Json::text("R4：现状 11,166,608 B + 1 MiB"));
    s.root.set("thresholds", th);
    Json med = Json::object();
    med.set("hostWallMs", Json::number(wall.median));
    med.set("hostWallMin", Json::number(wall.min));
    med.set("hostWallMax", Json::number(wall.max));
    med.set("insns", Json::u64(lift_stress::medianU64(insns_all)));
    med.set("heapInUse", Json::u64(lift_stress::medianU64(heap_all)));
    s.root.set("median", med);
    const fs::path written = s.write();

    std::printf("s1: 宿主墙钟中位 %.1f ms（%.1f/%.1f/%.1f，门槛 %.1f = 现状×1.5，R1）\n", wall.median, walls[0],
                walls[1], walls[2], kWallMedianMs);
    std::printf("s1: insns 三遍 %llu（极差 %llu，门槛 %llu，R2）；heap in_use %llu B（门槛 %llu，R4）\n",
                static_cast<unsigned long long>(insns_all[0]),
                static_cast<unsigned long long>(insns_max - insns_min),
                static_cast<unsigned long long>(kInsnsSpread),
                static_cast<unsigned long long>(heap_max), static_cast<unsigned long long>(kHeapInUseMax));
    std::printf("s1: 落盘 %s\n", written.string().c_str());
    return lift_test::finish("s1-boot-first-frame");
#endif
}
