// emulator-lift-aot · 入口 / CLI（Shape A）
//
// ★ 本文件是**新的** main：⛔ 不 include `machine.h`、⛔ 不实例化 `Machine`、⛔ 不调用
//   `Machine::run` / `BlockCache::get`。它只做四件事：
//     ① 定位执行源（**照 `src/main.cpp` 的默认 / 环境变量口径**，⛔ 不自己编路径）；
//     ② 调 `AotHost` 做平台 bring-up（映射 PE、建 guest 栈 / 堆 / TEB、装配宿主服务、注册并 bind shim）；
//        —— 其中 `loadImage` **先核对构建期 pin**（D1：sha256 + entry），不一致 ⇒ 打印两个哈希 + exit 2；
//     ③ 调 `AotHost::run` 跑到停机判据（`--max-frames` / `--max-insns` / `--until` / 停因）；
//     ④ 打印读数与运行摘要，按 `ok()` 给退出码（0 / 2，与解释器臂同一口径）。
//
// ★ 为什么执行源要跟解释器臂同一套解析：读数（base / entry / 节表 / 导入数）必须与
//   `emulator-lift` 报的是**同一份字节**，否则两边对不上时无法判断"是 lift 的问题还是喂了别的 exe"。
// ★ `getenv` 在 MSVC 口径里是 deprecated 警告（不是问题）；与 `src/main.cpp` 同一条处置。
#define _CRT_SECURE_NO_WARNINGS
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "aot/aot_dispatch.h"
#include "aot/aot_host.h"
#include "runtime/host_cursor.h"      // `--cursor-warp` 的平台闸门（与解释器臂同一条判据）
#include "runtime/host_encoding.h"
#include "runtime/input_plan.h"       // `--input-plan`（S4：AOT 臂唯一的输入控制面）
#include "runtime/present_flags.h"    // `--cursor-warp` / `--sound` 的**唯一**一份判据 + 文案
#include "runtime/present_sdl.h"
#include "runtime/vfs.h"  // openHostFileUtf8（Windows 的 UTF-8 路径）

namespace {

// ★ 与 `src/main.cpp` 的 `fileExists` 同一口径：Windows 上窄 `fopen` 按 ANSI 码页解释路径，
//   日文目录会打不开 ⇒ 必须走宽字符版。
bool fileExists(const std::string& p) {
    std::FILE* f = lift::openHostFileUtf8(p, "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

std::string envOr(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

void usage() {
    std::printf(
        "emulator-lift-aot —— Shape A：**执行**构建期 lift 出来的代码（⛔ 不构造 Machine）\n"
        "  --exe <path>        执行源（缺省：env LIFT_AGE_EXE / AMAYUI_AGE_EXE → 仓库内\n"
        "                      corpus/assets/age/AGE.EXE__dumped.sectfix.EXE；口径与 emulator-lift 相同）\n"
        "                      ★ 必须与构建期 pin 的 sha256 + entry 一致（D1）⇒ 不一致 exit 2\n"
        "  --game-dir <path>   guest 文件系统的唯一根（缺省 env AMAYUI_GAME_DIR / AMAYUI_INSTALL）\n"
        "  --guest-root <path> guest 眼里的安装根文本（缺省同 emulator-lift：E:\\Games\\Eushully\\天結）\n"
        "  --appdata <path>    伪造 AppData 的宿主目录（特殊目录映射；缺省 = 不用 AppData）\n"
        "  --max-frames N      Present 到第 N 帧后停（0 = 不限；缺省 0）\n"
        "  --max-insns N       guest 指令数绝对上限（0 = 不限）\n"
        "  --until EA          跑到该 EA（块边界）后停（hex，缺省关）\n"
        "  --time-limit SEC    墙钟上限（每 4096 条查一次；0 = 不限）\n"
        "  --skip-logo         一次性 latch：T_Command + 0x5EB5C 写 0（口径同 emulator-lift；\n"
        "                      钩子点 AOT 用 0x004BAA33；⛔ 不跳过 boot、不改 eip/寄存器）\n"
        "  --audio off|virtual|sdl  音频后端（本臂**只实现 off**：没有音频设备 / 喂料线程）\n"
        "  --input-plan <file> 组装输入计划（缺省不给 = 没有任何输入注入，行为与本参数无关）：\n"
        "                      文件格式 = `--interaction-graph` 的线性写法子集，门只有\n"
        "                      `await always` / `await presents <n>`（guest 累计 Present 帧数 ≥ n），\n"
        "                      动作只有 `click <x> <y> [left|right]` / `key <VK>`；\n"
        "                      推进只发生在 **guest 读点**（flush-before-read），⛔ 没有帧精度保证；\n"
        "                      跑完打一行 `input-plan: …` 摘要（一次性）。解析失败 ⇒ exit 2 + 行号\n"
        "%s"
        "  --dump-frame <path> 每次 Present 导出一份 guest 后缓冲 BMP（{frame} = 4 位帧号）\n"
        "  --profile           采样式内部 profile（**缺省关**）：每帧时间戳 + 派发 / lift_one / shim\n"
        "                      归因 + 机器频率校准；采样率 = dispatch 1/4096、lift_one 1/1024、\n"
        "                      shim 1/64（每个 shim 各自的调用序号；也写在输出头）。★ 采样本身会\n"
        "                      轻微影响时序 ⇒ 性能结论请用**关档**跑（⛔ 不拿 --profile 的墙钟当结论）；\n"
        "                      六项判据开/关一致\n"
        "  --frame-log         每次 Present 打一行 `[frame] n=<帧号> hash=<guest 后缓冲哈希>`（**缺省关**）：\n"
        "                      逐帧 hash 是画面序列读数（在动 / 卡住同一帧），不是判据；末帧 hash 看摘要\n"
        "                      `present :` 行。`--profile` 档也会打这一条（已有 profile 存档的可比面）\n"
        "  --region-log        启动 readout 末尾打 guest 区域表（`regions   :` 一行 + 每段一行，共 28 行；\n"
        "                      **缺省关**）：那是**静态布局快照**（谁占了哪一段），一次 run 里不变，\n"
        "                      属噪音 ⇒ 缺省档不打。⛔ 只影响这一块，别的 readout（exe / PE / 导入 /\n"
        "                      地址与服务面 / pin / 缺口 / 停机原因）一格都不收\n"
        "\n"
        "★ 呈现档位（口径与 emulator-lift 相同；缺省 headless）：\n"
        "  --present headless|window|auto\n"
        "                      headless = **隐藏**窗口 + GPU device（不是\"没有窗口\"：3D 的唯一光栅\n"
        "                      实现是 GPU，没有它一个像素都不画）；window = 可见窗口；\n"
        "                      auto = 可见窗口打不开 ⇒ 退到隐藏窗口\n"
        "  --present-driver LIST     SDL 渲染驱动列表（只对 window/auto 生效）\n"
        "  --present-vsync 0|1       垂直同步（缺省 0）\n"
        "  --present-hidden          强制隐藏窗口（仍必须显式 --present window/auto）\n"
        "  --present-screenshot <path>      每次可见窗口 Present 后存一张窗口 BMP（{frame} 可用）\n"
        "  --present-screenshot-frame N     只回读第 N 帧（1 起；0 = 每帧都写）\n"
        "%s"
        "  --help              这份说明\n"
        "\n"
        "★ 执行模型：入口 EA → 构建期生成的派发表（`lift_cc_<EA>`）；shim 桩 EA → `lift_shim`；\n"
        "  两者都不是 ⇒ **响亮停机并打印 EA**（⛔ 不设\"返回哨兵 = 结束\"的约定）。\n",
        lift::kAotSoundHelp, lift::kCursorWarpHelp);
}

// ★ 执行源定位**照抄** `src/main.cpp` 的四处顺序（--exe → env → 仓库内入库件 → 拒绝启动）。
//   ⛔ 不回退到游戏安装目录里的原版 exe（那是加壳件，节表未还原）。
bool resolveExePath(const std::string& explicit_path, std::string* out, std::vector<std::string>* tried) {
    if (!explicit_path.empty()) {
        *out = explicit_path;
        tried->push_back("--exe");
        return fileExists(*out);
    }
    // ★ main.cpp 的写法就是两个 env **拼起来**（只有同时设了两个才与"先后取一个"不同）；
    //   照抄是为了两处解析逐字一致。
    const std::string env = envOr("LIFT_AGE_EXE") + envOr("AMAYUI_AGE_EXE");
    if (!env.empty()) {
        *out = env;
        tried->push_back("env LIFT_AGE_EXE / AMAYUI_AGE_EXE");
        return fileExists(*out);
    }
    const char* candidates[] = {
        "corpus/assets/age/AGE.EXE__dumped.sectfix.EXE",
        "../../corpus/assets/age/AGE.EXE__dumped.sectfix.EXE",
    };
    for (const char* c : candidates) {
        tried->push_back(c);
        if (fileExists(c)) {
            *out = c;
            return true;
        }
    }
    return false;
}

bool parseU32Hex(const std::string& s, std::uint32_t* out) {
    const char* p = s.c_str();
    char* end = nullptr;
    const unsigned long long v = std::strtoull(p, &end, 16);
    if (end == p || *end != '\0' || v > 0xFFFFFFFFull) return false;
    *out = static_cast<std::uint32_t>(v);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    // ★ 进程入口三件事，顺序与 `src/main.cpp` 相同（理由见 runtime/memory.h / host_encoding.h）：
    //   ① 关掉 Windows 上"崩了就弹模态框"的默认行为（无人值守的整轮会被挂死）；
    //   ② 控制台输出码页 → UTF-8；
    //   ③ argv 一律按 UTF-8 取（CRT 的 char** 是 ANSI 码页）。
    lift::suppressCrashDialogsOnce();
    lift::initConsoleUtf8();
    const std::vector<std::string> args = lift::argvUtf8(argc, argv);

    std::string exe_path;
    std::string game_dir;
    std::string appdata_dir;
    std::string guest_root;
    // ★ `--input-plan <file>`（S4）：AOT 臂的**唯一**输入控制面（`runtime/input_plan.h`）。
    //   缺省空 = 不给 ⇒ 输出与行为逐字节不变（含那 57 行 stdout）。
    std::string input_plan_path;
    std::string audio_mode = "off";  // ★ 本臂缺省 off（没有音频设备）
    // ★ `--sound`（缺省关，与 `src/main.cpp:269` 同值）：本臂**接受**它，但只打一条一次性说明
    //   —— 这条臂没有音频管线（理由 / 后果 / 跟踪单只有一处：`runtime/present_flags.h`）。
    bool sound = false;
    lift::aot::AotRunOptions run_opts;
    // ---- 呈现档位（取值 / 缺省 / 使用门槛**照 `src/main.cpp:226-227`、`:357-394`、`:637-663`）----
    std::string present_mode = "headless";  // 与解释器同缺省
    std::string present_driver = lift::SdlWindowOptions{}.driver_list;
    bool present_driver_set = false;
    std::uint32_t present_vsync = 0;
    bool present_vsync_set = false;
    bool present_hidden = false;
    // ★ 宿主真光标（`--cursor-warp`，缺省关；与 `src/main.cpp:233` 同一个值）：只加宿主副作用，
    //   ⛔ 不改 guest 可见状态。只在 `--present window/auto` 且窗口可见时才真的挪（见
    //   `runtime/present_flags.h` 的 `cursorWarpState`）。
    bool cursor_warp = false;
    std::string present_screenshot;
    bool present_screenshot_set = false;
    std::uint64_t present_screenshot_frame = 0;
    std::string frame_dump;  // --dump-frame（guest 后缓冲 BMP；`cpu.shim.frame_dump`）

    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto needValue = [&](const char* flag, std::string* dst) {
            if (i + 1 >= args.size()) {
                std::fprintf(stderr, "emulator-lift-aot: %s 需要一个值\n", flag);
                return false;
            }
            *dst = args[++i];
            return true;
        };
        if (a == "--exe") {
            if (!needValue("--exe", &exe_path)) return 2;
        } else if (a == "--game-dir") {
            if (!needValue("--game-dir", &game_dir)) return 2;
        } else if (a == "--appdata") {
            if (!needValue("--appdata", &appdata_dir)) return 2;
        } else if (a == "--guest-root") {
            if (!needValue("--guest-root", &guest_root)) return 2;
        } else if (a == "--input-plan") {
            if (!needValue("--input-plan", &input_plan_path)) return 2;
        } else if (a == "--max-frames") {
            std::string v;
            if (!needValue("--max-frames", &v)) return 2;
            char* endp = nullptr;
            const unsigned long long n = std::strtoull(v.c_str(), &endp, 10);
            if (endp == v.c_str() || *endp != '\0') {
                std::fprintf(stderr, "emulator-lift-aot: --max-frames 需要非负整数：`%s`\n", v.c_str());
                return 2;
            }
            run_opts.max_frames = static_cast<std::uint32_t>(n);
        } else if (a == "--max-insns") {
            std::string v;
            if (!needValue("--max-insns", &v)) return 2;
            char* endp = nullptr;
            const unsigned long long n = std::strtoull(v.c_str(), &endp, 10);
            if (endp == v.c_str() || *endp != '\0') {
                std::fprintf(stderr, "emulator-lift-aot: --max-insns 需要非负整数：`%s`\n", v.c_str());
                return 2;
            }
            run_opts.max_insns = n;
        } else if (a == "--until") {
            std::string v;
            if (!needValue("--until", &v)) return 2;
            if (!parseU32Hex(v, &run_opts.until_ea)) {
                std::fprintf(stderr, "emulator-lift-aot: --until 需要一个 hex EA：`%s`\n", v.c_str());
                return 2;
            }
        } else if (a == "--time-limit") {
            std::string v;
            if (!needValue("--time-limit", &v)) return 2;
            char* endp = nullptr;
            const double d = std::strtod(v.c_str(), &endp);
            if (endp == v.c_str() || *endp != '\0' || d < 0.0) {
                std::fprintf(stderr, "emulator-lift-aot: --time-limit 需要非负数：`%s`\n", v.c_str());
                return 2;
            }
            run_opts.time_limit_sec = d;
        } else if (a == "--audio") {
            if (!needValue("--audio", &audio_mode)) return 2;
        } else if (a == "--sound") {
            // ★ 与 `src/main.cpp:519-520` 同一个 bool；本臂只做"接受 + 一条一次性说明"（见下）。
            sound = true;
        } else if (a == "--skip-logo") {
            // ★ 与 `src/main.cpp:301-302` 同一条：只置一个 bool（一次性 latch，语义在 aot_run.cpp）。
            run_opts.skip_logo = true;
        } else if (a == "--dump-frame") {
            // ★ 与 `src/main.cpp:1157` 同一个字段（`cpu.shim.frame_dump`）；D3D9 Present 消费。
            if (!needValue("--dump-frame", &frame_dump)) return 2;
        } else if (a == "--profile") {
            // ★ 采样式内部 profile（缺省关；采样率与三条硬口径见 `aot_host.h` 的 `AotProfile`）。
            run_opts.profile = true;
        } else if (a == "--frame-log") {
            // ★ 逐帧 `[frame] n=… hash=…`（**缺省关**；理由见 `AotRunOptions::frame_log`）。
            run_opts.frame_log = true;
        } else if (a == "--region-log") {
            // ★ 启动 readout 末尾的 guest 区域表 28 行（**缺省关**；理由见 `AotRunOptions::region_log`）。
            run_opts.region_log = true;
        } else if (a == "--present") {
            if (!needValue("--present", &present_mode)) return 2;
        } else if (a == "--present-driver") {
            if (!needValue("--present-driver", &present_driver)) return 2;
            present_driver_set = true;
        } else if (a == "--present-vsync") {
            std::string v;
            if (!needValue("--present-vsync", &v)) return 2;
            char* endp = nullptr;
            const unsigned long long n = std::strtoull(v.c_str(), &endp, 10);
            if (endp == v.c_str() || *endp != '\0' || n > 0xFFFFFFFFull) {
                std::fprintf(stderr, "emulator-lift-aot: --present-vsync 需要 0 / 1：`%s`\n", v.c_str());
                return 2;
            }
            present_vsync = static_cast<std::uint32_t>(n);
            present_vsync_set = true;
        } else if (a == "--present-hidden") {
            present_hidden = true;
        } else if (a == "--cursor-warp") {
            // ★ 与 `src/main.cpp:398-400` 同一条：只置一个 bool。窗口参数（headless 下 exit 2
            //   —— 没有可见窗口可映射）。
            cursor_warp = true;
        } else if (a == "--present-screenshot") {
            if (!needValue("--present-screenshot", &present_screenshot)) return 2;
            present_screenshot_set = true;
        } else if (a == "--present-screenshot-frame") {
            std::string v;
            if (!needValue("--present-screenshot-frame", &v)) return 2;
            char* endp = nullptr;
            const unsigned long long n = std::strtoull(v.c_str(), &endp, 10);
            if (endp == v.c_str() || *endp != '\0') {
                std::fprintf(stderr,
                             "emulator-lift-aot: --present-screenshot-frame 需要非负整数：`%s`\n", v.c_str());
                return 2;
            }
            present_screenshot_frame = n;
        } else if (a == "--help" || a == "-h") {
            usage();
            return 0;
        } else {
            std::fprintf(stderr, "emulator-lift-aot: 未知参数：%s\n", a.c_str());
            usage();
            return 2;
        }
    }
    // ★ 本臂**只实现 off**：没有音频设备 / 喂料线程。⛔ 不静默忽略 `--audio sdl`（那会让"以为有声"）。
    if (audio_mode != "off") {
        std::fprintf(stderr,
                     "emulator-lift-aot: --audio %s 未实现（本臂只有 off：没有音频设备 / 喂料线程）\n",
                     audio_mode.c_str());
        return 2;
    }

    // ---- `--sound`：**接受** + 一条一次性说明（⛔ 不 exit 2、⛔ 不静默）----
    //   ★ 裁决与硬事实只在 `runtime/present_flags.h` 的 `kAotSoundNotice`（含理由 / 后果 / 跟踪单）。
    //   ★ 打印点**只有这里**（parse 之后、GPU bring-up 之前）⇒ 一次，且命令行一给就能判
    //     （守卫 `cli-aot-flags` 因此不需要 GPU）。
    //   ★ 放在 `--audio` 拒绝**之后**：那句"其余行为不受影响"不能被一次 `--audio sdl` 的 exit 2
    //     打脸（⛔ 本臂不因为 `--sound` 拒绝启动，但别的硬拒绝照旧）。
    if (sound) std::fputs(lift::kAotSoundNotice, stderr);

    // ---- `--input-plan`：**解析在 GPU bring-up 之前**（CLI 判据不需要 GPU）----
    //   ★ 文件不存在 / 语法错 / 门种类不支持 ⇒ **exit 2 + 点名行号与原因**（⛔ 不许静默忽略）。
    lift::InputPlan input_plan;
    if (!input_plan_path.empty()) {
        std::string plan_err;
        if (!input_plan.loadFile(input_plan_path, &plan_err)) {
            std::fprintf(stderr, "emulator-lift-aot: --input-plan 载入失败：%s\n", plan_err.c_str());
            return 2;
        }
    }

    // ---- 呈现后端参数校验（口径**逐条**照 `src/main.cpp:637-676`；⛔ 不发明第三种语义）----
    // ★ `--cursor-warp` 是**平台**能力：Linux / 其它平台没有实现 ⇒ 启动即拒绝（exit 2）。
    //   判据与文案与解释器臂**同一处**（`runtime/present_flags.h`），⛔ 不在这里另写一句。
    if (cursor_warp && !lift::hostCursorPlatformSupported()) {
        std::fprintf(stderr, lift::kCursorWarpUnsupportedFormat, "emulator-lift-aot",
                     lift::hostCursorPlatformName());
        return 2;
    }
    if (present_mode != "headless" && present_mode != "window" && present_mode != "auto") {
        std::fprintf(stderr, "emulator-lift-aot: --present 只能是 headless / window / auto（收到 %s）\n",
                     present_mode.c_str());
        return 2;
    }
    const bool present_window_flag = present_driver_set || present_vsync_set || present_hidden ||
                                     present_screenshot_set || cursor_warp;
    if (present_screenshot_frame != 0 && present_screenshot.empty()) {
        std::fprintf(stderr, "--present-screenshot-frame 需要同时给 --present-screenshot <path>\n");
        return 2;
    }
    if (present_mode == "headless" && present_window_flag) {
        std::fprintf(stderr, lift::kHeadlessRejectsWindowFlagsFormat, lift::kWindowFlagsAot);
        return 2;
    }
    if (present_mode != "headless" && present_driver.find("software") != std::string::npos) {
        std::fprintf(stderr,
                     "emulator-lift-aot: ⚠ --present-driver 含 software ⇒ 这是**软件渲染**，不是 GPU 路径"
                     "（只用于测试 / 诊断）\n");
    }

    // =====================================================================================
    // 显示 / GPU 光栅化后端（真源 `src/main.cpp:720-786` 的**同一条**路径）
    //
    // ★ `--present headless` **不是"没有窗口"**：解释器也建一个**隐藏**窗口来拿 GPU device
    //   （`main.cpp:724-726`），因为 GPU 是**唯一**的光栅实现 —— 拿不到它，`d3d9_pipeline.cpp:3983`
    //   会整批拒批，3D 一个像素都不画（W0 C10 Ⓐ）。⇒ 本臂两个档位都建窗口，只有 visible/hidden 之分。
    // ★ 无 GPU ⇒ **拒绝启动**（与解释器同一条硬失败，⛔ 没有"这一轮先不画"的降级）。
    // =====================================================================================
    std::unique_ptr<lift::SdlWindow> display;
    {
        lift::SdlWindowOptions wopt;
        wopt.title = "emulator-lift-aot";
        wopt.driver_list = present_driver;
        wopt.vsync = present_vsync != 0;
        // headless / --present-hidden ⇒ 隐藏窗口（`--present headless` 不出现可见窗口）。
        wopt.hidden = present_mode == "headless" || present_hidden;
        wopt.screenshot_path = present_screenshot;
        wopt.screenshot_frame = present_screenshot_frame;
        // ★ 与解释器臂同一个字段（`main.cpp:742`）：缺这一行 ⇒ sink 恒 `cursor_warp=false`
        //   ⇒ `hostCursorWarpEnabled()` 恒假 ⇒ shim 只改虚拟光标（用户看到的"不会移动鼠标"）。
        wopt.cursor_warp = cursor_warp;
        wopt.raster_gpu = true;
        auto sdl = std::make_unique<lift::SdlWindow>();
        std::string werr;
        bool ok = sdl->init(wopt, &werr);
        if (!ok && present_mode == "auto") {
            // `--present auto`：可见窗口打不开（无桌面）⇒ 退到隐藏的离屏窗口（GPU 仍必须成立）。
            std::fprintf(stderr,
                         "emulator-lift-aot: --present auto：可见窗口打不开（%s）⇒ 改用隐藏的离屏窗口\n",
                         werr.c_str());
            if (!present_screenshot.empty()) {
                std::fprintf(stderr, "  ⚠ --present-screenshot 不会产出（没有可见窗口可抓）\n");
            }
            wopt.hidden = true;
            werr.clear();
            ok = sdl->init(wopt, &werr);
        }
        // ★ "我开了但没生效"必须响亮一次（与解释器臂同一句话、同一判据；见
        //   `runtime/present_flags.h`）：`--present-hidden` 与 auto 的隐藏回退走同一条 stderr warn。
        //   ⛔ 不改成 exit 2：隐藏窗口是有意的无窗口复现档（`main.cpp:761-764` 的同一裁决）。
        if (ok && lift::cursorWarpState(cursor_warp, lift::hostCursorPlatformSupported(), wopt.hidden) ==
                      lift::CursorWarpState::OnButHidden) {
            std::fputs(lift::kCursorWarpHiddenWarn, stderr);
        }
        if (ok && !sdl->rasterActive()) {
            werr = "GPU device 建出来了，但光栅化后端未激活（`rasterActive()==false`）";
        }
        if (!ok || !sdl->rasterActive()) {
            std::fprintf(stderr,
                         "emulator-lift-aot: 拒绝启动 —— 没有可用的 GPU 光栅化后端\n"
                         "  原因：%s\n"
                         "  本线只有 GPU 一个光栅实现（CPU 光栅已退休）；没有 GPU 就不启动。\n"
                         "  请检查：\n"
                         "    · 图形后端 / 驱动是否可用（Windows: D3D12 / Vulkan；macOS: Metal）；\n"
                         "    · 是否在**没有桌面**的环境里跑（远程会话 / 服务 / `SDL_VIDEODRIVER=dummy`）：\n"
                         "      SDL 必须先初始化视频子系统才能建 GPU device ⇒ 这种环境请换到有桌面的会话里跑。\n"
                         "  ⛔ 本线不提供无 GPU 时的降级路径（`--present headless` 也要 GPU）。\n",
                         werr.empty() ? "GPU 后端创建失败（SDL 没有给出原因）" : werr.c_str());
            return 2;
        }
        display = std::move(sdl);
    }
    // ★ 指向那个窗口对象的**稳定**指针：window 档位稍后把 `display` move 进 presenter 的 sink，
    //   但对象本身仍活着（所有权转给 presenter）⇒ describe() / attachWindowManager / 事件泵都走它。
    lift::SdlWindow* const sdl_display = display.get();

    // ---- 定位执行源（与 emulator-lift 同一套顺序；⛔ 不回退到安装树里的加壳原件）----
    std::vector<std::string> tried;
    {
        std::string resolved;
        if (!resolveExePath(exe_path, &resolved, &tried)) {
            std::fprintf(stderr, "emulator-lift-aot: 找不到执行源（拆壳件的字节）。找过：\n");
            for (const std::string& t : tried) std::fprintf(stderr, "  - %s\n", t.c_str());
            std::fprintf(stderr, "  用 --exe <path> 指定，或先 `git lfs pull`（入库件在 corpus/assets/age/）。\n");
            return 2;
        }
        exe_path = resolved;
    }

    // ---- guest 文件系统的根（口径同 main.cpp：--game-dir → env → 本机安装树探测）----
    if (game_dir.empty()) game_dir = envOr("AMAYUI_GAME_DIR");
    if (game_dir.empty()) game_dir = envOr("AMAYUI_INSTALL");
    if (game_dir.empty()) {
        const char* fallback = "E:\\Games\\Eushully\\天結";
        if (fileExists(std::string(fallback) + "\\AGE.EXE")) game_dir = fallback;
    }

    lift::aot::AotHost host;
    std::string err;

    // ---- ① 平台 + guest 地址空间 + 宿主服务装配 ----
    if (!host.init(&err)) {
        std::fprintf(stderr, "emulator-lift-aot: 宿主装配失败：%s\n", err.c_str());
        return 1;
    }
    // ★ GPU 光栅化后端：**必须在第一条 guest 指令之前**接到宿主服务上（口径 `main.cpp:1171` 的
    //   `machine.setGpuRaster(display->gpuRaster())`）。d3d9 / DDraw 的第一件事就是查它；
    //   拿不到 ⇒ `d3d9_pipeline.cpp:3983` 整批拒批（3D 一个像素都不画，`~4544` 不再 CPU 重放）。
    host.setGpuRaster(sdl_display->gpuRaster());
    // ★ VFS 的三条配置必须在 loadImage/prepare 之前（prepare 里的 `setupProcessStructures` 会用
    //   `vfs.guestExePath()` 与 `vfs.guestEnvironment()` 造 guest 的命令行 / 环境块）。
    if (!game_dir.empty()) host.vfs().setGameDir(game_dir);
    if (!appdata_dir.empty()) host.vfs().setAppDataDir(appdata_dir);
    if (!guest_root.empty()) host.vfs().setGuestRoot(guest_root);

    // ---- ② 载入 PE（**先核对 D1 的 pin**，再映射进 guest）----
    if (!host.loadImage(exe_path, &err)) {
        if (host.pinMismatch()) {
            // ★ D1：打印两个哈希 + exit 2（⛔ 不降级、不警告后继续）。
            std::fprintf(stderr, "emulator-lift-aot: exe pin 核对失败\n%s\n", err.c_str());
            return 2;
        }
        std::fprintf(stderr, "emulator-lift-aot: 载入 PE 失败（%s）：%s\n", exe_path.c_str(), err.c_str());
        return 1;
    }

    // ---- ③ 装配 guest 进程结构 + shim + cpu 初始寄存器 ----
    if (!host.prepare(&err)) {
        std::fprintf(stderr, "emulator-lift-aot: 平台 bring-up 失败：%s\n", err.c_str());
        return 1;
    }

    // ---- ③'' `--input-plan` 装配（`WindowManager` = 唯一状态源；`presenter` = presents 门）----
    //   ★ 两处都只是**非拥有指针**。计划不给 ⇒ 两处都不装配 ⇒ 读点只付一条判空、输出逐字节不变。
    //   ★ ⛔ 不装在 `aot_run.cpp` / `machine.cpp` 里（守卫 ① 钉住那 5 个词 = 0 命中）。
    if (input_plan.armed()) {
        host.cpu().windows->setInputPlan(&input_plan);
        host.presenter().setInputPlan(&input_plan);
    }

    // ---- ③' AGERC.DLL：真实 guest 模块（口径同 `src/main.cpp`：`--game-dir` 下那份，走 VFS 解析）----
    //   ★ 缺它不是"可选项"：guest 启动早期就 `LoadLibraryA("AGERC.DLL")`，没有镜像 ⇒ 返回 0 + 126
    //     ⇒ guest 走错误对话框路径（实测停在 `poly:005C3627 = j_MessageBoxA_15`）。
    //   ⛔ 与解释器臂同口径：**降级但不假装成功**（打一行 warn，LoadLibrary 自己会返回 0 + 126）。
    if (host.vfs().ready()) {
        std::string agerc_host;
        std::string verr;
        if (host.vfs().resolve("AGERC.DLL", &agerc_host, &verr, true, true)) {
            if (!host.loadAgercModule(agerc_host, &err)) {
                std::fprintf(stderr,
                             "emulator-lift-aot: ⚠ AGERC.DLL 装载失败 ⇒ 继续（降级为\"没有 AGERC 镜像\"）\n"
                             "  %s\n  ⇒ LoadLibrary(\"AGERC.DLL\") 会返回 0 + 错误 126\n",
                             err.c_str());
            }
        } else {
            std::fprintf(stderr,
                         "emulator-lift-aot: ⚠ --game-dir 下没有可解析的 AGERC.DLL（%s）⇒ "
                         "LoadLibrary(\"AGERC.DLL\") 会返回 0 + 错误 126\n",
                         verr.c_str());
        }
    }

    // ---- ④ 生成物在不在场：**响亮**报告（⛔ 不许静默跑成空壳）----
    if (!lift::aot::generatedPresent()) {
        std::fprintf(stderr,
                     "\n"
                     "⛔ emulator-lift-aot: **没有生成物**\n"
                     "   构建期缺 `clang` / 执行源（corpus/assets/age/AGE.EXE__dumped.sectfix.EXE）/\n"
                     "   `aot/closure.txt` 的任一项 ⇒ 这个 exe 里**没有任何 lifted 函数**，派发表为空\n"
                     "   （构建仍然成功，但**不能执行**）。⛔ 不静默跑成空壳：只打 bring-up 读数后 exit 1。\n"
                     "   修：装上 clang（`clang --version`）并确认执行源在场后重新 configure / build。\n\n");
        std::printf("emulator-lift-aot: Shape A —— bring-up 读数（**没有生成物，未执行任何 guest 指令**）\n");
        host.printReadout(stdout, run_opts.region_log);
        std::fflush(stdout);
        return 1;
    }

    // ---- ④' AOT **形状**进二进制 + 启动第一屏一行（设计 §4.1-4）----
    //   `LIFT_AOT_SHAPE` 由 CMake 从 `LIFT_AOT_SPLIT` + `-O2` 派生：判据档只允许
    //   `single-O2` / `splitN-O2` 两种写法；将来真有 dev 档，它的值必须带 `-NOT-A-JUDGE`
    //   ⇒ 这里同时打警示，让人**无法**在 dev 档上得出性能结论。
    {
        const char* shape = LIFT_AOT_SHAPE;
        std::printf("emulator-lift-aot: AOT 形状 LIFT_AOT_SHAPE=\"%s\"（split=%d · -O2）\n", shape,
                    static_cast<int>(LIFT_AOT_SPLIT));
        if (std::strstr(shape, "-NOT-A-JUDGE") != nullptr) {
            std::printf("⚠ 非判据档（⛔ 性能结论不可用）\n");
        }
    }

    // ---- 宿主诊断配置：帧导出 / 帧数上限（口径同 `src/main.cpp:1156-1162`；不进 guest 语义）----
    host.cpu().shim.frame_dump = frame_dump;
    host.cpu().shim.max_frames = run_opts.max_frames;
    if (!frame_dump.empty()) {
        std::printf("frame-dump: %s（每次 Present 导出 guest 后缓冲 BMP；{frame} = 4 位帧号）\n",
                    frame_dump.c_str());
    }

    // ---- ⑤ 窗口 sink 装配（只在 `--present window/auto`；口径 `src/main.cpp:1199-1220`）----
    //   ★ 三件事缺一不可（W0 B7）：
    //     ① `presenter().setSink(...)`：像素真的送到窗口；
    //     ② `attachWindowManager`：SDL 事件才翻得成 guest 的 `WM_*`（没有它事件只更新 stop）；
    //     ③ `setHostEventPump`：guest 阻塞在 `GetMessageA`（空队列）时代替它等事件 ——
    //        ⛔ 不装 ⇒ `user32_window.cpp:589` 的 `shimAbort` → `std::abort()`（不可归因停机）。
    bool present_window_active = false;
    if (present_mode != "headless") {
        if (!sdl_display->isGpuRenderer()) {
            std::printf("present   : 注意：driver=%s 是 GPU API 路径，但不是 SDL_GPU（gpu driver）\n",
                        sdl_display->rendererName());
        }
        host.presenter().setSink(std::move(display));  // 所有权转给 presenter（与 main.cpp:1208 同）
        sdl_display->attachWindowManager(host.cpu().windows);
        // ★ AOT 没有输入脚本 / 交互图（那是解释器臂的宿主控制面）⇒ 泵里只有 SDL 事件。
        host.cpu().windows->setHostEventPump(
            [sdl_display](lift::X86& /*cpu*/) { sdl_display->pumpHostEvents(5); });
        present_window_active = true;
    }

    std::printf("emulator-lift-aot: Shape A —— bring-up 完成，开始执行（派发表 %zu 项）\n",
                lift::aot::tableSize());
    // ★ 启动回声：两行都是**读数**（⛔ 不写死）：
    //   · `raster` = 真正的光栅后端名（GPU 是唯一实现）；
    //   · `present` = sink 的 `describe()`，其中 `window=visible|hidden` 是"这轮看得见吗"的机械判据
    //     （取自 `SDL_GetWindowFlags`；headless 也建隐藏窗口 ⇒ 这一行**两个档位都打**）。
    std::printf("raster    : %s%s\n", host.gpuRaster() != nullptr ? host.gpuRaster()->rasterName() : "(nullptr)",
                present_mode == "headless" ? "（headless 离屏；**隐藏**窗口，不出现可见窗口）" : "");
    std::printf("present   : %s\n", sdl_display->describe().c_str());
    // ★ `--cursor-warp` 的**有效状态**一行（缺省关）：`on` / `off` / `on-but-hidden` / `unsupported`。
    //   与 `describe()` 里的 `cursor-warp=on|off`（sink 自己的开关）分工不同 —— 这一行回答
    //   "这轮真的会不会挪宿主光标"（含"开关开着但窗口是隐藏的"这种静默失效）。
    //   ★ 判据与四个状态名只有一份（`runtime/present_flags.h`），与解释器臂逐字相同。
    {
        const lift::CursorWarpState st =
            lift::cursorWarpState(cursor_warp, lift::hostCursorPlatformSupported(), sdl_display->windowHidden());
        std::printf("cursor-warp: %s\n", lift::cursorWarpStateName(st));
    }
    host.printReadout(stdout, run_opts.region_log);
    std::fflush(stdout);

    // ---- ⑥ 执行 ----
    const lift::aot::AotRunResult r = host.run(run_opts);
    // ★ 与解释器 `main.cpp:1269` 同一条收口：先摘事件泵，再打摘要（⛔ 不让泵在收口后碰窗口）。
    if (present_window_active) host.cpu().windows->clearHostEventPump();
    // ---- ⑥' `--input-plan` 收口：**跑结束前必须抬**（否则下一次 run 会看到"键一直按着"）----
    //   `releaseAll()` 只抬起待办的键 / 按钮，不是 step、不推进入口；返回个数只用于摘要。
    if (input_plan.armed()) {
        input_plan.releaseAll(*host.cpu().windows);
        printf("input-plan: %s\n", input_plan.summary().c_str());
        if (input_plan.stepsApplied() < input_plan.steps()) {
            // 响亮但不改退出码（⛔ 本单不新增停止原因）：还没到期的 step 是**配方问题**，不是成功。
            std::fprintf(stderr,
                         "emulator-lift-aot: ⚠ input-plan 还有 %llu 步没到期（共 %llu 步；"
                         "看 `presents` 门的目标帧数是否超过本轮 `--max-frames`）\n",
                         (unsigned long long)(input_plan.steps() - input_plan.stepsApplied()),
                         (unsigned long long)input_plan.steps());
        }
        std::fflush(stdout);
    }
    host.printRunSummary(r, stdout);
    if (r.reason == lift::aot::AotStop::DecodeError) {
        // ★ "EA 不在表里 / 不是 shim" 这一类**必须**在 stderr 上也响亮一次（stdout 的摘要可能被管道吞）。
        std::fprintf(stderr, "emulator-lift-aot: ⛔ 停在一个派发表里没有的 EA：%s\n", r.detail.c_str());
    }
    std::fflush(stdout);
    std::fflush(stderr);
    // ★ 退出码口径与解释器臂逐字相同（`machine.cpp:109` 的 `exitCodeForRun`）：ok ⇒ 0，否则 2。
    return r.ok() ? 0 : 2;
}
