// emulator-lift · 入口 / CLI
//
// ★ 默认路径（README §2.1 的定位顺序）：`--exe` → 环境变量 → 仓库内入库件 → 拒绝启动。
//   ⛔ 不回退到游戏安装目录里的原版 exe（那是 binary/age-original 的兄弟，节表未还原）。
// ★ 启动前校验：PE 的 sha256 必须等于 `.lst` 头部的 `; Input SHA256 :`（plan §4 D1 / A3 ⓪）。
//   不符 ⇒ 打印两侧并拒绝启动；`.lst` 缺失 ⇒ 打印"无法校验"（⛔ 不假装校验过）。
// ★ 用法：
//     emulator-lift --smoke
//     emulator-lift [--exe <path>] [--lst <path>] [--until shim | --until ea <hex>]
//                   [--skip-logo] [--max-insns N] [--trace-shims] [--trace-blocks] [--safe-memory] [--bench N]
#include <SDL3/SDL.h>

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "corpus/lst_index.h"
#include "runtime/font.h"
#include "runtime/guest_token.h"  // --assert-guest-token（单令牌违规计数）
#include "runtime/host_cursor.h"  // --cursor-warp 的平台闸门
#include "runtime/host_encoding.h"
#include "runtime/log.h"
#include "runtime/machine.h"
#include "runtime/present_flags.h"  // --cursor-warp 的判据 + 文案（与 AOT 臂**同一份**）
#include "runtime/present_sdl.h"
#include "runtime/sha256.h"
#include "runtime/vfs.h"  // openHostFileUtf8（Windows 的 UTF-8 路径）

namespace {

bool fileExists(const std::string& p) {
    // ★ UTF-8 路径：Windows 上必须走宽字符版（窄 fopen 按 ANSI 代码页解释 ⇒ 日文目录打不开）。
    std::FILE* f = lift::openHostFileUtf8(p, "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

std::string envOr(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

std::uint32_t parseHex(const char* s) {
    if (!s) return 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    return static_cast<std::uint32_t>(std::strtoul(s, nullptr, 16));
}

void usage() {
    std::printf(
        "emulator-lift —— 在 guest-state x86 上执行 AGE.EXE 的机器码（M1a）\n"
        "  --exe <path>        执行源（缺省：env LIFT_AGE_EXE → 仓库内 corpus/assets/age/…sectfix.EXE）\n"
        "  --lst <path>        .lst 语料（诊断 / A3 对照；缺省自动找 corpus/disasm/files/…）\n"
        "  --game-dir <path>   guest 文件系统的**唯一根**（S6 文件与路径族；缺省 env AMAYUI_GAME_DIR）\n"
        "  --guest-root <path> guest 眼里的安装根文本（缺省 E:\\Games\\Eushully\\天結；只用于路径前缀）\n"
        "  --appdata <path>    伪造 AppData 的宿主目录（特殊目录映射；缺省 = 不用 AppData）\n"
        "                      ⚠ 视作**可丢弃**的伪造根：启动时会清 <path>\\Local\\Temp（A4 确定性）\n"
        "  --agerc <path>      显式指定 AGERC.DLL（真实 guest 模块装载）。缺省 = --game-dir/AGERC.DLL\n"
        "                      （安装树里的是**汉化免壳件**）；两段都拿不到 ⇒ LoadLibrary(\"AGERC.DLL\")\n"
        "                      返回 0 + 126。⛔ 不回退 DATA1.ALF 的条目（引擎本身也不回退；\n"
        "                      那条约仅供取证对照，不是装载来源）\n"
        "  --real-time         用宿主时钟（此时 A4 的逐字节断言自动跳过；trace 输入清单标\n"
        "                      clock=REAL-TIME，⛔ 不再额外打印警告行）\n"
        "  --skip-logo         把引擎自己的 show-logo 标志置 0（根脚本 0x130 因而不 call LOGO；\n"
        "                      仍走完整 boot 到 TITLE，⛔ 不是直接跳到 TITLE 的宿主捷径）\n"
        "  --accept-recovery-dialog  恢复引擎真实的「前回正常終了しませんでした」判定\n"
        "                      （**默认绕过**：等价于「上次正常结束」= marker 不存在；传它才会走到\n"
        "                      恢复框。⛔ 不是替用户答 NO；默认档 MessageBoxA **一次都不会被调**）\n"
        "  --close-confirm <ask|exit|title|cancel>  X（关窗）的二次确认策略（缺省 ask）：\n"
        "                      ask = 弹系统「終了」框（1=終了 / 2=タイトルに戻る / 0=キャンセル；\n"
        "                      Esc/X=0）；exit/title/cancel = 不弹框，直接按该选择返回（exit 即旧行为）\n"
        "  --trace-inputs      打印 trace 输入清单（exe 路径串 / guest 命令行 / 环境块 / 时钟起点）\n"
        "  --until shim        跑到第一次 shim 调用就停（L1）\n"
        "  --until ea <hex>    跑到指定 EA 就停（L2 = 0x4BA890）\n"
        "  --max-insns <N>     最多执行 N 条指令\n"
        "  --max-frames <N>    最多呈现 N 帧后停（0 = 不限；D3D9 Present 计数）\n"
        "  --dump-frame <path> 每次 Present 导出 BMP；路径里的 {frame} 换成 4 位帧号\n"
        "                      （不含 {frame} 时同一文件会被覆盖；配合 --max-frames 1 取第一帧）\n"
        "  --time-limit <秒>   墙钟上限（探索用；0 = 不限，缺省）\n"
        "  --log-level <lvl>   quiet|error|warn|info|debug|trace（缺省 warn；Error 永远显示）\n"
        "  --log-cat <list>    只显示这些分类（all,general,shim,file,vfs,com,d3d9,d3dx9,dsound,\n"
        "                      winmm,video,loader,native,report；缺省 all）\n"
        "  --verbose           = --log-level info\n"
        "  --trace-shims       打印 shim 调用（确定性格式；仍受 --log-cat 过滤）\n"
        "  --trace-blocks      打印每条执行的指令（调试用，很慢）\n"
        "  --safe-memory       每次访存都查提交位图（A1 的第二遍）\n"
        "  --report            打印基座缺口（A5）后退出\n"
        "  --bench <N>         跑 N 条指令并报 MIPS\n"
        "  --profile           跑完后打印热点（guest 块级 + 按 .lst 函数聚合 + 宿主 shim 墙钟；\n"
        "                      shim 表与 guest 块表不可相加）\n"
        "  --profile-top <N>   profile 每张表打印前 N 项（缺省 30）\n"
        "  --present <mode>    headless（缺省）/ window / auto：呈现后端；window = SDL 窗口 + GPU API 路径\n"
        "  --present-driver <list>  逗号列表（缺省 gpu,direct3d12,direct3d11,vulkan,metal,opengl）\n"
        "  --present-vsync 0|1       窗口垂直同步（缺省 0；1 会改变墙钟）\n"
        "  --present-hidden          隐藏窗口（CTest / 无人值守）\n"
        "  --present-screenshot <path>  窗口像素 BMP（present 前回读；支持 {frame}）\n"
        "  --present-screenshot-frame <N>  只回读第 N 个 Present（1 起；缺省 0 = 每帧都写）\n"
        "  --overlay           窗口左上角调试叠层（鼠标 guest 客户区坐标 / fps / frame / raster / present；\n"
        "                      默认关；只改宿主送显的窗口像素，⛔ 不写 guest 内存、不进 --dump-frame）\n"
        "%s"
        "  ⛔ 2026-11-16 起**时间线输入整族退役**：`--input-script` / `--anchor-script` / `--input-stall-ms` /\n"
        "     `--input-stall-boot-ms` / `--input-max-hold` / `--input-marker-timeout-ms` / `--expect-change-after` /\n"
        "     `--expect-change-window` 都不再是参数（给了 ⇒ 一行「已退役」+ exit 2）。理由与重开条件见\n"
        "     `docs/03-emulator/input-plan.md` §7：输入只经 AOT 臂的 `--input-plan`（配方），旧 bench 一并退役）。\n"
        "  --audio off|sdl|virtual  音频后端（缺省 sdl = 真设备；virtual = 确定性设备时钟；off = silent）\n"
        "  --audio-dump <path>  把混音结果写成 WAV（44.1k/16/2；与 sdl 同时用 = tee）\n"
        "  --sound              允许出声（缺省静音：设备/混音/喂料/guest DS 调用照跑，只在交付前置 0）\n"
        "  --assert-guest-token  运行结束若单令牌违规计数 > 0（或有 >1 个令牌持有者）⇒ exit 4\n"
        "                      （判据 = summary 的 `token : violations=0`；见 runtime/guest_token.h）\n"
        "                      或窗口状态被非主线程访问（summary 的 `window : off-thread=0`）⇒ exit 4\n"
        "                      （WindowManager 是主线程独占宿主对象，见 runtime/host_window.h）\n"
        "  --font <path>       宿主字体（缺省：CMake 注入的 Amayui CN；调试覆盖）\n"
        "  --native-overrides <list>  逗号分隔的 guest 函数名 / 段级项名（如 sub_414AC0）；all = 已验证项；\n"
        "                             -名字 = 排除该项；也可显式叠加调试项（缺省关）\n"
        "  --baseline-lift <N> 宿主基线补偿像素（缺省 2；0 = 关；★ 宿主视觉策略，不是引擎语义）\n"
        "  --font-engine-name  字体 A/B 对照档：上报引擎字面名（字形仍来自宿主 Amayui CN）\n"
        "  --smoke             只验证宿主层可链接（不建窗口、不需要游戏）\n",
        lift::kCursorWarpHelp);
}

}  // namespace

int main(int argc, char** argv) {
    // ★ 第一件事：关掉 Windows 上"崩了就弹模态框"的默认行为（见 runtime/memory.h）。
    //   本仓有多处 `std::abort()`（heap 非法 free / 句柄校验 / shimAbort），只关其中一条不够 ——
    //   对话框会把无人值守的整轮挂死，而且 stdout 还没 flush ⇒ 连日志都看不到。
    //   ★ 必须在 argvUtf8() 的分配之前：OOM 也属于"崩了"。
    lift::suppressCrashDialogsOnce();
    // ★ 第二件事：Windows 控制台输出码页 → UTF-8（见 host_encoding.h）。
    //   必须在任何 printf 之前 —— 否则 usage / 诊断文本会按 936 显示成乱码。
    lift::initConsoleUtf8();
    // ★ 第三件事：argv 一律按 UTF-8 取（Windows 的 CRT char** 是 ANSI 码页，日文 --game-dir
    //   会被当成 GBK 字节再按 UTF-8 解析 ⇒ fs::path 抛异常、进程 abort）。
    const std::vector<std::string> args = lift::argvUtf8(argc, argv);
    const int arg_count = static_cast<int>(args.size());
    bool smoke = false;
    bool trace_shims = false;
    bool trace_blocks = false;
    bool safe_memory = false;
    bool report = false;
    bool real_time = false;
    bool skip_logo = false;
    bool accept_recovery_dialog = false;
    lift::CloseConfirmPolicy close_confirm = lift::CloseConfirmPolicy::Ask;
    bool trace_inputs = false;
    std::string log_level = "warn";
    std::string log_cats;
    bool log_level_set = false;
    std::uint64_t bench = 0;
    bool profile = false;
    std::size_t profile_top = 30;
    std::uint32_t max_insns = 0;
    std::uint32_t max_frames = 0;
    std::string frame_dump;
    double time_limit_sec = 0.0;
    std::uint32_t until_ea = 0;
    bool until_shim = false;
    std::string exe_path;
    std::string lst_path;
    std::string game_dir;
    std::string appdata_dir;
    std::string guest_root;
    std::string agerc_path;
    std::string font_path;
    std::string native_overrides;
    std::string present_mode = "headless";
    std::string present_driver = lift::SdlWindowOptions{}.driver_list;
    std::string present_screenshot;
    std::uint64_t present_screenshot_frame = 0;
    // ★ 窗口调试叠层（`--overlay`，默认关）：只画进宿主送显的窗口像素，⛔ 不碰 guest 内存。
    bool overlay = false;
    // ★ 宿主真光标（`--cursor-warp`，默认关）：只加宿主副作用，⛔ 不改 guest 可见状态。
    bool cursor_warp = false;
    // ★ 方案 D：默认 SDL 后端 + **静音**（`--sound` 才出声；设备 / 混音 / 喂料照跑）；
    //   `--audio=virtual` = 确定性设备时钟（CTest / dump）；`--audio=off` = 旧 silent 口径。
    //   `--audio-dump` 与真输出可同时用（tee）。
    // ★ `--sound` 是**输出门**，与后端正交：缺省静音（全流程照跑、只在交付前置 0）。
    std::string audio_mode = "sdl";
    std::string audio_dump;
    bool sound = false;
    int present_vsync = 0;
    bool present_hidden = false;
    bool present_driver_set = false;
    bool present_vsync_set = false;
    bool present_screenshot_set = false;
    bool assert_guest_token = false;
    int baseline_lift = lift::kBaselineLift;  // 宿主视觉策略；单一常量在 font.h（对齐 cxx 的 text_raster.cpp:647）
    bool font_engine_name = false;

    for (int i = 1; i < arg_count; ++i) {
        const char* a = args[static_cast<std::size_t>(i)].c_str();
        if (std::strcmp(a, "--smoke") == 0) {
            smoke = true;
        } else if (std::strcmp(a, "--exe") == 0 && i + 1 < arg_count) {
            exe_path = args[++i];
        } else if (std::strcmp(a, "--lst") == 0 && i + 1 < arg_count) {
            lst_path = args[++i];
        } else if (std::strcmp(a, "--game-dir") == 0 && i + 1 < arg_count) {
            game_dir = args[++i];
        } else if (std::strcmp(a, "--appdata") == 0 && i + 1 < arg_count) {
            appdata_dir = args[++i];
        } else if (std::strcmp(a, "--agerc") == 0 && i + 1 < arg_count) {
            agerc_path = args[++i];
            if (agerc_path.empty()) {
                std::fprintf(stderr, "--agerc 需要一个非空路径\n");
                return 2;
            }
        } else if (std::strcmp(a, "--guest-root") == 0 && i + 1 < arg_count) {
            guest_root = args[++i];
        } else if (std::strcmp(a, "--real-time") == 0) {
            real_time = true;
        } else if (std::strcmp(a, "--skip-logo") == 0) {
            skip_logo = true;
        } else if (std::strcmp(a, "--accept-recovery-dialog") == 0) {
            accept_recovery_dialog = true;
        } else if (std::strcmp(a, "--close-confirm") == 0 && i + 1 < arg_count) {
            if (!lift::parseCloseConfirmPolicy(args[++i], &close_confirm)) {
                std::fprintf(stderr, "--close-confirm 需要 ask|exit|title|cancel（收到 \"%s\"）\n",
                             args[i].c_str());
                return 2;
            }
        } else if (std::strcmp(a, "--trace-inputs") == 0) {
            trace_inputs = true;
        } else if (std::strcmp(a, "--until") == 0 && i + 1 < arg_count) {
            const char* what = args[++i].c_str();
            if (std::strcmp(what, "shim") == 0) {
                until_shim = true;
            } else if (std::strcmp(what, "ea") == 0 && i + 1 < arg_count) {
                until_ea = parseHex(args[++i].c_str());
            } else {
                std::fprintf(stderr, "--until 需要 shim 或 ea <hex>\n");
                return 2;
            }
        } else if (std::strcmp(a, "--max-insns") == 0 && i + 1 < arg_count) {
            max_insns = static_cast<std::uint32_t>(std::strtoull(args[++i].c_str(), nullptr, 10));
        } else if (std::strcmp(a, "--max-frames") == 0 && i + 1 < arg_count) {
            max_frames = static_cast<std::uint32_t>(std::strtoull(args[++i].c_str(), nullptr, 10));
        } else if (std::strcmp(a, "--dump-frame") == 0 && i + 1 < arg_count) {
            frame_dump = args[++i];
            if (frame_dump.empty()) {
                std::fprintf(stderr, "--dump-frame 需要一个非空路径\n");
                return 2;
            }
        } else if (std::strcmp(a, "--time-limit") == 0 && i + 1 < arg_count) {
            // ★ `0` = **不设墙钟上限**，与省略该参数**完全等价**（三处同一口径：usage 的
            //   "0 = 不限，缺省" / 本行 / `Machine::run` 的 `opts.time_limit_sec > 0.0` 判据）。
            //   修前这里是 `<= 0.0` ⇒ usage 说"0 = 不限"，实际 exit 2（explore 脚本写死的
            //   `--time-limit 0` 整轮跑不起来）。⛔ 不要写回 `<= 0.0`（help 会重新变成骗人）。
            //   负数 / 非数字仍然是 exit 2（消息说清是哪个 flag + 收到了什么）。
            const char* text = args[++i].c_str();
            char* end = nullptr;
            const double secs = std::strtod(text, &end);
            if (end == text || *end != '\0' || !std::isfinite(secs) || secs < 0.0) {
                std::fprintf(stderr, "--time-limit 需要非负数秒（0 = 不限；收到 %s）\n", text);
                return 2;
            }
            time_limit_sec = secs;
        } else if (std::strcmp(a, "--bench") == 0 && i + 1 < arg_count) {
            bench = std::strtoull(args[++i].c_str(), nullptr, 10);
        } else if (std::strcmp(a, "--profile") == 0) {
            profile = true;
        } else if (std::strcmp(a, "--profile-top") == 0 && i + 1 < arg_count) {
            profile_top = static_cast<std::size_t>(std::strtoull(args[++i].c_str(), nullptr, 10));
            if (profile_top == 0) {
                std::fprintf(stderr, "--profile-top 需要正整数\n");
                return 2;
            }
        } else if (std::strcmp(a, "--present") == 0 && i + 1 < arg_count) {
            present_mode = args[++i];
        } else if (std::strcmp(a, "--present-driver") == 0 && i + 1 < arg_count) {
            present_driver = args[++i];
            present_driver_set = true;
            if (present_driver.empty()) {
                std::fprintf(stderr, "--present-driver 需要非空列表\n");
                return 2;
            }
        } else if (std::strcmp(a, "--present-vsync") == 0 && i + 1 < arg_count) {
            const char* text = args[++i].c_str();
            char* end = nullptr;
            const long value = std::strtol(text, &end, 10);
            present_vsync_set = true;
            if (end == text || *end != '\0' || (value != 0 && value != 1)) {
                std::fprintf(stderr, "--present-vsync 只能是 0 或 1（收到 %s）\n", text);
                return 2;
            }
            present_vsync = static_cast<int>(value);
        } else if (std::strcmp(a, "--present-hidden") == 0) {
            present_hidden = true;
        } else if (std::strcmp(a, "--present-screenshot") == 0 && i + 1 < arg_count) {
            present_screenshot = args[++i];
            present_screenshot_set = true;
            if (present_screenshot.empty()) {
                std::fprintf(stderr, "--present-screenshot 需要一个非空路径\n");
                return 2;
            }
        } else if (std::strcmp(a, "--present-screenshot-frame") == 0 && i + 1 < arg_count) {
            const char* text = args[++i].c_str();
            char* end = nullptr;
            const unsigned long long v = std::strtoull(text, &end, 10);
            if (end == text || *end != '\0' || v == 0) {
                std::fprintf(stderr, "--present-screenshot-frame 需要正整数（收到 %s）\n", text);
                return 2;
            }
            present_screenshot_frame = static_cast<std::uint64_t>(v);
            present_screenshot_set = true;  // 同属窗口参数：headless 下拒绝
        } else if (std::strcmp(a, "--overlay") == 0) {
            // 调试叠层：只作用于 `--present window`（headless 没有窗口可画）。
            overlay = true;
        } else if (std::strcmp(a, "--cursor-warp") == 0) {
            // 宿主真光标：缺省关；窗口参数（headless 下 exit 2 —— 没有可见窗口可映射）。
            cursor_warp = true;
        } else if (std::strcmp(a, "--input-script") == 0 || std::strcmp(a, "--anchor-script") == 0 ||
                   std::strcmp(a, "--input-stall-ms") == 0 || std::strcmp(a, "--input-stall-boot-ms") == 0 ||
                   std::strcmp(a, "--input-max-hold") == 0 ||
                   std::strcmp(a, "--input-marker-timeout-ms") == 0 ||
                   std::strcmp(a, "--expect-change-after") == 0 ||
                   std::strcmp(a, "--expect-change-window") == 0) {
            // ★ 2026-11-16 用户裁决：**时间线输入整族退役**（`REQ-01M7HQ57XV66FZQA5ECGHWD71N`）。
            //   ⛔ 不静默忽略：旧脚本 / 旧 bench 的调用方必须当场看到"这条路没了"（exit 2）。
            //   ⛔ 也不为它补一套"新参数名"：输入的用户面只有 AOT 臂的 `--input-plan`（配方）。
            std::fprintf(stderr,
                         "emulator-lift: %s 已退役（2026-11-16）——解释器臂没有输入时间线入口了；\n"
                         "  输入的用户面只有 AOT 臂 `--input-plan`（配方 = apps/emulator-lift/scripts/recipes/*.plan）。\n"
                         "  理由 / 重开条件见 docs/03-emulator/input-plan.md §7\n",
                         a);
            return 2;
        } else if (std::strcmp(a, "--audio") == 0 && i + 1 < arg_count) {
            audio_mode = args[++i];
            if (audio_mode != "off" && audio_mode != "sdl" && audio_mode != "virtual") {
                std::fprintf(stderr, "--audio 只能是 off / sdl / virtual（收到 %s）\n", audio_mode.c_str());
                return 2;
            }
        } else if (std::strcmp(a, "--audio-dump") == 0 && i + 1 < arg_count) {
            audio_dump = args[++i];
            if (audio_dump.empty()) {
                std::fprintf(stderr, "--audio-dump 需要一个非空路径\n");
                return 2;
            }
        } else if (std::strcmp(a, "--sound") == 0) {
            sound = true;
        } else if (std::strcmp(a, "--assert-guest-token") == 0) {
            assert_guest_token = true;
        } else if (std::strcmp(a, "--trace-shims") == 0) {
            trace_shims = true;
        } else if (std::strcmp(a, "--trace-blocks") == 0) {
            trace_blocks = true;
        } else if (std::strcmp(a, "--log-level") == 0 && i + 1 < arg_count) {
            log_level = args[++i];
            log_level_set = true;
            if (!lift::logSetLevel(log_level.c_str())) {
                std::fprintf(stderr, "--log-level 只能是 quiet/error/warn/info/debug/trace（收到 %s）\n",
                             log_level.c_str());
                return 2;
            }
        } else if (std::strcmp(a, "--log-cat") == 0 && i + 1 < arg_count) {
            log_cats = args[++i];
            if (!lift::logSetCategories(log_cats.c_str())) return 2;
        } else if (std::strcmp(a, "--verbose") == 0) {
            log_level = "info";
            log_level_set = true;
            lift::logSetLevel("info");
        } else if (std::strcmp(a, "--safe-memory") == 0) {
            safe_memory = true;
        } else if (std::strcmp(a, "--font") == 0 && i + 1 < arg_count) {
            font_path = args[++i];
        } else if (std::strcmp(a, "--native-overrides") == 0 && i + 1 < arg_count) {
            native_overrides = args[++i];
            if (native_overrides.empty()) {
                std::fprintf(stderr, "--native-overrides 需要非空列表（关掉就整个参数都不要给）\n");
                return 2;
            }
        } else if (std::strcmp(a, "--baseline-lift") == 0 && i + 1 < arg_count) {
            baseline_lift = std::atoi(args[++i].c_str());
            if (baseline_lift < 0 || baseline_lift > 64) {
                std::fprintf(stderr, "--baseline-lift 需要 0..64（收到 %d）\n", baseline_lift);
                return 2;
            }
        } else if (std::strcmp(a, "--font-engine-name") == 0) {
            font_engine_name = true;
        } else if (std::strcmp(a, "--report") == 0) {
            report = true;
        } else if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0) {
            usage();
            return 0;
        } else {
            std::fprintf(stderr, "未知参数：%s\n", a);
            usage();
            return 2;
        }
    }

    // ---- ★ `--dump-frame` 的目录：**解析完参数就建**（REQ-01M73YBCC3DZ05Y9NSDTRKTMMY）----
    //   为什么必须在跑之前：BMP 是在**每帧 Present** 里写的；目录不存在时 `dumpBmp` 每次
    //   `fopen` 都失败。旧实现把这次失败当成 Present 失败 ⇒ `d3d9::present` 在到达
    //   `--max-frames` 的停机检查**之前**就返回了 ⇒ 进程一直跑到别的停止条件
    //   （实测 546 s；目录建好后同一条命令 7 s —— 取证 agent 两次误以为模拟器卡死）。
    //   ⇒ 建不出来（权限 / 路径非法 / 中间被文件挡住）就**当场** exit 2：
    //   ⛔ 不静默、⛔ 不"跑到第一帧再说"。
    //   ★ `frame_{frame}.bmp` 这种没有目录成分的写法：`parent_path()` 为空 ⇒ 跳过
    //     （当前目录必然存在，⛔ 不要去 create 一个空路径）。
    if (!frame_dump.empty()) {
#if defined(_WIN32)
        // ★ `--dump-frame` 是 UTF-8（argvUtf8 的重建口径）；Windows 的窄 path 会按 ANSI 码页解释
        //   ⇒ 日文目录建错。⇒ 先转宽字符（与 `openHostFileUtf8` 同一口径）。
        const std::filesystem::path dump_path(lift::utf8ToWide(frame_dump));
#else
        const std::filesystem::path dump_path(frame_dump);
#endif
        const std::filesystem::path dump_dir = dump_path.parent_path();
        if (!dump_dir.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(dump_dir, ec);
            if (ec) {
                std::fprintf(stderr,
                             "emulator-lift: --dump-frame 的目录建不出来，拒绝启动\n"
                             "  path : %s\n"
                             "  err  : %s（error_code=%d）\n"
                             "  提示：换成可写目录，或先手工建好它（本程序只负责建目录，不猜你要写到哪）\n",
                             frame_dump.c_str(), ec.message().c_str(), ec.value());
                return 2;
            }
        }
    }

    // ---- 日志配置（CLI 优先；env 只在 CLI 没给时生效）----
    if (!log_level_set) {
        if (const char* env = std::getenv("LIFT_LOG")) {
            log_level = env;
            if (!lift::logSetLevel(log_level.c_str())) {
                std::fprintf(stderr, "LIFT_LOG=%s 非法（quiet/error/warn/info/debug/trace）\n", env);
                return 2;
            }
        }
    }
    if (log_cats.empty()) {
        if (const char* env = std::getenv("LIFT_LOG_CAT")) {
            log_cats = env;
            if (!lift::logSetCategories(log_cats.c_str())) return 2;
        }
    }
    // ★ 输出档位（`REQ-01M7AG06MXS6PK2B53B29QM76Z`）：默认只打"结论 / 异常 / 一行读数摘要"；
    //   `--profile` / `--report` / `--log-level info` 及以上（含 `--verbose` 与 `LIFT_LOG=info`）
    //   才打全量启动回声与逐项读数。⛔ 这是**输出**档位，不参与任何 guest 语义 / trace hash。
    const bool verbose_output = profile || report || lift::logConfig().level >= lift::LogLevel::Info;
    // ---- 呈现后端参数校验（缺省 headless；窗口参数给了就必须显式 --present window/auto）----
    if (present_mode != "headless" && present_mode != "window" && present_mode != "auto") {
        std::fprintf(stderr, "--present 只能是 headless / window / auto（收到 %s）\n", present_mode.c_str());
        return 2;
    }
    // ★ `--cursor-warp` 是**平台**能力：Linux / 其它平台没有实现 ⇒ 启动即拒绝（exit 2）。
    //   理由（design §5）：这个开关的全部价值就是"一种真实副作用"；平台给不出 ⇒ 整轮运行的前提
    //   不成立，而在 30 分钟后从日志里发现"光标一次都没动"是最差的形状。实现层仍然有第二道防线
    //   （`hostCursorWarpToScreen` 在非 win32/macOS 上返回 false）。
    if (cursor_warp && !lift::hostCursorPlatformSupported()) {
        // ★ 判据与文案与 AOT 臂**同一处**（`runtime/present_flags.h`），⛔ 不在这里另写一句。
        std::fprintf(stderr, lift::kCursorWarpUnsupportedFormat, "emulator-lift",
                     lift::hostCursorPlatformName());
        return 2;
    }
    const bool present_window_flag = present_driver_set || present_vsync_set || present_hidden ||
                                     present_screenshot_set || overlay || cursor_warp;
    if (present_screenshot_frame != 0 && present_screenshot.empty()) {
        std::fprintf(stderr, "--present-screenshot-frame 需要同时给 --present-screenshot <path>\n");
        return 2;
    }
    if (present_mode == "headless" && present_window_flag) {
        std::fprintf(stderr, lift::kHeadlessRejectsWindowFlagsFormat, lift::kWindowFlagsInterpreter);
        return 2;
    }
    if (present_mode != "headless" && present_driver.find("software") != std::string::npos) {
        std::fprintf(stderr,
                     "emulator-lift: ⚠ --present-driver 含 software ⇒ 这是**软件渲染**，不是 GPU 路径"
                     "（只用于测试 / 诊断）\n");
    }
    if (present_mode != "headless" && smoke) {
        std::fprintf(stderr, "--smoke 不建窗口；不要同时给 --present window/auto\n");
        return 2;
    }
    if (present_mode != "headless" && report) {
        std::fprintf(stderr, "--report 只打印基座缺口；不要同时给 --present window/auto\n");
        return 2;
    }

    if (smoke) {
        if (!SDL_Init(0)) {
            std::fprintf(stderr, "emulator-lift: SDL_Init failed: %s\n", SDL_GetError());
            return 1;
        }
        const int linked = SDL_GetVersion();
        std::printf("emulator-lift 0.1.0 (SDL %d.%d.%d)\n", SDL_VERSIONNUM_MAJOR(linked), SDL_VERSIONNUM_MINOR(linked),
                    SDL_VERSIONNUM_MICRO(linked));
        SDL_Quit();
        std::printf("smoke ok\n");
        return 0;
    }

    if (safe_memory) {
#if defined(_WIN32)
        _putenv_s("LIFT_SAFE_MEMORY", "1");
#else
        setenv("LIFT_SAFE_MEMORY", "1", 1);
#endif
    }

    // ---- 宿主字体策略（P2 / SCOPE）：默认 Amayui CN；CLI 只做调试覆盖 ----
    {
        lift::FontConfig font = lift::fontDefaultConfig();
        if (!font_path.empty()) {
            font.regular_path = font_path;
            font.bold_path.clear();  // 由 regular_path 派生 `-Bold.ttf`
        }
        font.baseline_lift = baseline_lift;
        font.use_engine_name = font_engine_name;
        lift::fontSetConfig(font);
        // ★ 启动回声默认不逐行刷（`REQ-01M7AG06MXS6PK2B53B29QM76Z`）：字体是**配置回显**，
        //   不属于"结论 / 异常 / 一行读数"；`--profile` / `--report` / `--log-level info` 才打。
        if (verbose_output) {
            std::printf("font      : %s（baseline-lift=%d%s）\n",
                        lift::fontConfig().regular_path.empty() ? "(未配置 ⇒ 文本 API 会明确失败)"
                                                               : lift::fontConfig().regular_path.c_str(),
                        lift::fontConfig().baseline_lift,
                        lift::fontConfig().use_engine_name ? "；A/B 对照档（只影响上报族名）" : "");
        }
    }

    // =====================================================================================
    // GPU 光栅化后端：**唯一的光栅实现**（`REQ-01M78BF1FNNH6K8N8GEY6GQYXX`）
    //
    // ★ 无 GPU ⇒ **拒绝启动**（非 0 退出 + 指路），⛔ 不再有"这一轮先用另一套光栅"的静默降级。
    // ★ `--present headless` 也建一个**隐藏**的离屏窗口：SDL 要先初始化视频子系统才拿得到
    //   GPU device（`SDL_VIDEODRIVER=dummy` 下 `SDL_CreateGPUDevice` 必然失败 ⇒ 那台机器按
    //   "无 GPU"处理）。⛔ 不出现可见窗口（`SdlWindowOptions::hidden`）。
    // ★ 放在执行源定位**之前**：没有 GPU 是**启动期**硬失败，不需要先载入游戏才知道
    //   （守卫 `lift/no-gpu-refuses-start` 因此可以在没有任何游戏资产的机器上跑）。
    //   `--report` / `--smoke` 是纯宿主诊断（不需要窗口），仍不要求 GPU。
    // =====================================================================================
    std::unique_ptr<lift::SdlWindow> display;
    if (!report) {
        lift::SdlWindowOptions wopt;
        wopt.title = "emulator-lift";
        wopt.driver_list = present_driver;
        wopt.vsync = present_vsync != 0;
        // headless / --present-hidden ⇒ 隐藏窗口（`--present headless` 不出现可见窗口）。
        wopt.hidden = present_mode == "headless" || present_hidden;
        wopt.screenshot_path = present_screenshot;
        wopt.screenshot_frame = present_screenshot_frame;
        wopt.overlay = overlay;
        wopt.cursor_warp = cursor_warp;
        wopt.raster_gpu = true;
        auto sdl = std::make_unique<lift::SdlWindow>();
        std::string werr;
        bool ok = sdl->init(wopt, &werr);
        if (!ok && present_mode == "auto") {
            // `--present auto`：可见窗口打不开（无桌面）⇒ 退到**隐藏**的离屏窗口。
            // ⛔ 这不是"退回另一套光栅"：GPU 仍必须成立，建不出 device 就在下面拒绝启动。
            std::fprintf(stderr,
                         "emulator-lift: --present auto：可见窗口打不开（%s）⇒ 改用隐藏的离屏窗口\n",
                         werr.c_str());
            if (!present_screenshot.empty()) {
                std::fprintf(stderr, "  ⚠ --present-screenshot 不会产出（没有可见窗口可抓）\n");
            }
            wopt.hidden = true;
            wopt.overlay = false;
            werr.clear();
            ok = sdl->init(wopt, &werr);
        }
        // ★ "我开了但没生效"必须响亮（需求判据 3）：显式 `--present-hidden` 与 `--present auto` 的
        //   隐藏回退走**同一句** stderr warn。上一版只有 auto 分支打，README 却写成两条路径都
        //   "给 warn"（复审阻 2c）。⛔ 不改成 exit 2：`--present-hidden` 是有意的无窗口复现档，
        //   且 auto 的回退发生在 init 之后，parse 期没法 exit 2（需求判据 3 对"窗口隐藏"只要求 warn）。
        if (ok && lift::cursorWarpState(cursor_warp, lift::hostCursorPlatformSupported(), wopt.hidden) ==
                      lift::CursorWarpState::OnButHidden) {
            std::fputs(lift::kCursorWarpHiddenWarn, stderr);
        }
        if (ok && !sdl->rasterActive()) {
            werr = "GPU device 建出来了，但光栅化后端未激活（`rasterActive()==false`）";
        }
        if (!ok || !sdl->rasterActive()) {
            std::fprintf(stderr,
                         "emulator-lift: 拒绝启动 —— 没有可用的 GPU 光栅化后端\n"
                         "  原因：%s\n"
                         "  本线只有 GPU 一个光栅实现（CPU 光栅已退休）；没有 GPU 就不启动。\n"
                         "  请检查：\n"
                         "    · 图形后端 / 驱动是否可用（macOS: Metal；Windows: D3D12 / Vulkan；Linux: Vulkan）；\n"
                         "    · 是否在**没有桌面**的环境里跑（远程会话 / 服务 / `SDL_VIDEODRIVER=dummy`）：\n"
                         "      SDL 必须先初始化视频子系统才能建 GPU device ⇒ 这种环境请换到有桌面的会话里跑。\n"
                         "  ⛔ 本线不提供无 GPU 时的降级路径（`--present headless` 也要 GPU）。\n",
                         werr.empty() ? "GPU 后端创建失败（SDL 没有给出原因）" : werr.c_str());
            return 2;
        }
        display = std::move(sdl);
    }

    // ---- 定位执行源（四处，顺序固定；都不在 ⇒ 拒绝启动）----
    std::vector<std::string> tried;
    if (exe_path.empty()) {
        const std::string env = envOr("LIFT_AGE_EXE") + envOr("AMAYUI_AGE_EXE");
        if (!env.empty()) {
            exe_path = env;
            tried.push_back("env LIFT_AGE_EXE / AMAYUI_AGE_EXE");
        }
    }
    if (exe_path.empty()) {
        const char* candidates[] = {
            "corpus/assets/age/AGE.EXE__dumped.sectfix.EXE",
            "../../corpus/assets/age/AGE.EXE__dumped.sectfix.EXE",
        };
        for (const char* c : candidates) {
            tried.push_back(c);
            if (fileExists(c)) {
                exe_path = c;
                break;
            }
        }
    }
    if (exe_path.empty() || !fileExists(exe_path)) {
        std::fprintf(stderr, "emulator-lift: 找不到执行源（拆壳件的字节）。找过：\n");
        for (const std::string& t : tried) std::fprintf(stderr, "  - %s\n", t.c_str());
        std::fprintf(stderr,
                     "  用 --exe <path> 指定，或先 `git lfs pull`（入库件在 corpus/assets/age/）。\n"
                     "  ⛔ 不回退到游戏安装目录里的原版 exe。\n");
        return 2;
    }

    // ---- .lst（诊断与 A3 对照；缺了不致命，但必须明说）----
    if (lst_path.empty()) {
        const char* candidates[] = {
            "corpus/disasm/files/AGE.EXE__dumped.sectfix.EXE.lst",
            "../../corpus/disasm/files/AGE.EXE__dumped.sectfix.EXE.lst",
        };
        for (const char* c : candidates) {
            if (fileExists(c)) {
                lst_path = c;
                break;
            }
        }
    }
    const bool have_lst = !lst_path.empty() && fileExists(lst_path);
    // ★ 执行源与 `.lst` 头部一致时置位：默认档把它折进 `image` 行的 `lst=ok`（⛔ 不再单独占一行）。
    bool lst_sha_ok = false;

    lift::Machine machine;
    std::string err;
    if (!machine.init(&err)) {
        std::fprintf(stderr, "emulator-lift: 建 guest 地址空间失败：%s\n", err.c_str());
        return 1;
    }
    {
        const lift::AudioMode am = audio_mode == "off" ? lift::AudioMode::Off
                                  : audio_mode == "virtual" ? lift::AudioMode::Virtual
                                                            : lift::AudioMode::Sdl;
        if (!machine.configureAudio(am, audio_dump, sound, &err)) {
            std::fprintf(stderr, "emulator-lift: 音频配置失败：%s\n", err.c_str());
            return 2;
        }
    }    if (game_dir.empty()) game_dir = envOr("AMAYUI_GAME_DIR");
    if (game_dir.empty()) game_dir = envOr("AMAYUI_INSTALL");
    if (game_dir.empty()) {
        const char* fallback = "E:\\Games\\Eushully\\天結";
        if (fileExists(std::string(fallback) + "\\AGE.EXE")) game_dir = fallback;
    }
    if (!game_dir.empty()) {
        machine.vfs().setGameDir(game_dir);
        // ★ 配置回显只在 Verbose 打（默认档的启动回声只留"关键身份"：exe / image / agerc / raster / present）。
        if (verbose_output) std::printf("game-dir  : %s\n", game_dir.c_str());
    } else {
        std::fprintf(stderr,
                     "emulator-lift: ⚠ 没有 --game-dir（也没有 AMAYUI_GAME_DIR）：文件与路径族会**响亮失败**\n"
                     "  （S6：guest 路径只映射到 --game-dir；⛔ 不回退到任意宿主目录）\n");
    }
    if (!appdata_dir.empty()) {
        machine.vfs().setAppDataDir(appdata_dir);
        if (verbose_output)
            std::printf("appdata   : %s（伪造 AppData；特殊目录映射到这里）\n", appdata_dir.c_str());
    }
    if (!guest_root.empty()) machine.vfs().setGuestRoot(guest_root);
    if (!machine.loadImage(exe_path, &err)) {
        std::fprintf(stderr, "emulator-lift: 载入 PE 失败（%s）：%s\n", exe_path.c_str(), err.c_str());
        return 1;
    }

    // ---- A3 ⓪：字节 ↔ 语料 的对齐校验 ----
    if (have_lst) {
        if (!machine.loadLst(lst_path, &err)) {
            std::fprintf(stderr, "emulator-lift: 载入 .lst 失败（%s）：%s\n", lst_path.c_str(), err.c_str());
            return 1;
        }
        const std::string lst_sha = lift::toLower(machine.lst().headerSha256());
        const std::string pe_sha = lift::toLower(machine.image().sha256);
        if (lst_sha.empty()) {
            std::fprintf(stderr, "emulator-lift: .lst 头部没有 `; Input SHA256 :`，无法校验字节来源\n");
            return 1;
        }
        if (lst_sha != pe_sha) {
            std::fprintf(stderr,
                         "emulator-lift: 拒绝启动 —— 执行源与语料不是同一份二进制\n"
                         "  PE   sha256 = %s  (%s)\n"
                         "  .lst sha256 = %s  (%s)\n",
                         pe_sha.c_str(), exe_path.c_str(), lst_sha.c_str(), lst_path.c_str());
            return 1;
        }
        lst_sha_ok = true;
        // ★ 默认档不再单独打 `sha256 ok`：同一份 sha 已经在 `image` 行上（并带 `lst=ok`）。
        if (verbose_output) std::printf("sha256 ok  : %s（与 .lst 头部一致）\n", pe_sha.c_str());
    } else {
        std::fprintf(stderr,
                     "emulator-lift: ⚠ 没有找到 .lst —— 无法校验执行源与语料是否同一份（plan §4 D1）；"
                     "诊断里也不会有指令文本。\n");
    }

    // ---- guest 函数级 native override（试点；默认关）----
    if (!native_overrides.empty()) {
        std::string ov_err;
        if (!machine.configureNativeOverrides(native_overrides, &ov_err)) {
            std::fprintf(stderr, "emulator-lift: --native-overrides 配置失败：%s\n", ov_err.c_str());
            return 2;
        }
        // ★ 默认档**只打汇总行**；per-entry 只在 `--profile` / `--report` / `--log-level info`
        //   及以上时打（摘要里的 per-entry 另有"异常条目必须留"的判据，见
        //   `Machine::dumpRunSummary`）。启动时还没有 hits，所以这里没有"异常条目"可留。
        //   ⛔ `[debug]` override 的 stderr 警告**保留**（它不在这条门控里）。
        std::printf("native-ovr: %zu 项 + %zu 段级（--native-overrides=%s）\n", machine.overrides().size(),
                    machine.blockOverrides().size(), native_overrides.c_str());
        for (std::size_t i = 0; i < machine.overrides().size(); ++i) {
            const lift::NativeOverride* e = machine.overrides().at(i);
            const lift::KnownNative* k = lift::findKnownNative(e->name);
            if (verbose_output)
                std::printf("  %-12s @ %08X %s(%uB)%s\n", e->name.c_str(), e->ea,
                            lift::guestConvName(e->conv), e->arg_bytes,
                            (k != nullptr && k->debug) ? " [debug]" : "");
            if (k != nullptr && k->debug) {
                std::fprintf(stderr,
                             "emulator-lift: ⚠ debug native override %s：已知未收口，仅用于复现/调试；"
                             "不要把它当作 A4 逐字节基线\n",
                             e->name.c_str());
            }
        }
        for (std::size_t i = 0; i < machine.blockOverrides().size(); ++i) {
            const lift::NativeBlockOverride* e = machine.blockOverrides().at(i);
            if (verbose_output)
                std::printf("  %-12s @ %08X..%08X 段级（函数中段；跳到出口）\n", e->name.c_str(),
                            e->entry_ea, e->resume_ea);
        }
        if (until_ea != 0) {
            std::fprintf(stderr,
                         "emulator-lift: ⚠ --until ea 与被 override 的函数体不兼容："
                         "函数体会整段被 native 实现替代，内部 EA 不会逐条命中。\n");
        }
        if (bench != 0) {
            std::fprintf(stderr,
                         "emulator-lift: ⚠ --bench + native override：insns / MIPS 按"
                         "\"一次 override = 1 条\"计，不是解释器绝对吞吐。\n");
        }
    }

    if (const char* dump = std::getenv("LIFT_DUMP_EA")) {
        const std::uint32_t ea = parseHex(dump);
        std::printf("[dump] mem[%08X] = %08X (region %s)\n", ea, machine.mem().rd32(ea),
                    machine.mem().regionOf(ea) ? lift::regionKindName(machine.mem().regionOf(ea)->kind) : "none");
        std::printf("[dump] file bytes: ");
        const std::uint32_t rva = ea - machine.image().image_base;
        for (const lift::PeSectionInfo& s : machine.image().sections) {
            if (rva >= s.va && rva < s.va + s.vsize && (rva - s.va) < s.raw_size) {
                const std::size_t off = s.raw_offset + (rva - s.va);
                for (int i = 0; i < 4 && off + i < machine.image().bytes.size(); ++i)
                    std::printf("%02X", machine.image().bytes[off + i]);
                std::printf("  (section %s raw_off %X)\n", s.name, s.raw_offset);
            }
        }
    }
    // ★ 启动回声合并（`REQ-01M7AG06MXS6PK2B53B29QM76Z`）：exe / image base / entry / sha256 一行
    //   （默认档还要把 agerc 并进**同一行**，见下面 AGERC 装载处）；节表 11 行只在 Verbose 展开
    //   （默认档只留 `sections=N`）。机器可读字段一个不丢。
    if (verbose_output) {
        std::printf("image      : %s base=%08X entry=%08X sections=%zu imports=%zu sha256=%s%s\n",
                    exe_path.c_str(), machine.image().image_base, machine.image().entry_ea,
                    machine.image().sections.size(), machine.image().imports.size(),
                    machine.image().sha256.c_str(), lst_sha_ok ? " lst=ok" : "");
        for (const lift::PeSectionInfo& s : machine.image().sections) {
            std::printf("  %-8s va=%08X vsize=%8X raw=%8X flags=%08X\n", s.name, s.va, s.vsize, s.raw_size,
                        s.characteristics);
        }
    }

    if (!machine.prepare(0, &err)) {
        std::fprintf(stderr, "emulator-lift: 装配 guest 进程结构失败：%s\n", err.c_str());
        return 1;
    }
    machine.setSkipLogo(skip_logo);
    // ★ 默认**绕过**恢复判定（= "marker 不存在" = 上次正常结束）；传 flag 才走引擎真实判定。
    machine.setAcceptRecoveryDialog(accept_recovery_dialog);
    // ★ X 的二次确认策略（AGERC `_ShowDialog@12` id==3）：缺省 ask = 真弹系统「終了」框。
    machine.setCloseConfirmPolicy(close_confirm);

    // ★ 2026-11-16：`--input-script` 整族（时间线 / 0 点 / 效果级判据）的 CLI 入口已退役
    //   （`REQ-01M7HQ57XV66FZQA5ECGHWD71N`）。`InputScript` 类本身**保留**为机器内动作落地面：
    //   交互配方驱动器（`InteractionGraph::actuator`）与 pure CTest 夹具还要用它 ⇒ 见报告
    //   `.tmp/graphfix/report-idescript.md` 的依赖清单。⛔ 不要在这里补第二套输入语义。

    // ★ guest 眼里的主模块路径（用户 2026-11-05 口径）：`<guest 根>\AGE.EXE`，
    //   打开它拿到的是**我们真正在跑的那份字节**（拆壳件），不是 `--game-dir` 下的加壳原件。
    //   默认档的 `image` 行已经带 exe 路径 ⇒ 这条配置回显只在 Verbose 打。
    if (verbose_output) {
        std::printf("exe(guest): %s（VFS overlay ⇒ %s）\n", machine.vfs().overlayGuestPath().c_str(),
                    machine.vfs().overlayHostPath().c_str());
    }

    // ---- AGERC.DLL：作为**真实 guest 模块**装载（用户 2026-11-04 改口径）----
    //   显式 `--agerc <path>`；否则 `--game-dir` 下的 AGERC.DLL（走 VFS 解析，大小写不敏感）。
    //   显式给了但装载失败 ⇒ 拒绝启动（exit 2）；缺省路径失败 ⇒ 警告并继续
    //   （降级成"没有 AGERC 镜像"：LoadLibrary 返回 0 + 126，⛔ 不假装成功）。
    const bool agerc_explicit = !agerc_path.empty();
    if (!agerc_explicit && machine.vfs().ready()) {
        std::string host;
        std::string verr;
        if (machine.vfs().resolve("AGERC.DLL", &host, &verr, true, true)) agerc_path = host;
    }
    if (!agerc_path.empty()) {
        std::string agerc_err;
        if (!machine.loadAgercModule(agerc_path, &agerc_err)) {
            if (agerc_explicit) {
                std::fprintf(stderr, "emulator-lift: --agerc 装载失败，拒绝启动\n  %s\n",
                             agerc_err.c_str());
                return 2;
            }
            std::fprintf(stderr,
                         "emulator-lift: ⚠ --game-dir 下的 AGERC.DLL 装载失败 ⇒ 继续（降级为"
                         "\"没有 AGERC 镜像\"）\n  %s\n"
                         "  ⇒ LoadLibrary(\"AGERC.DLL\") 会返回 0 + 错误 126\n",
                         agerc_err.c_str());
        } else if (verbose_output) {
            // ★ Verbose 的独立一行（原来两行）；默认档见下面合并的 `image … | agerc=…` 一行。
            const lift::PeImage& ai = machine.agercImage();
            std::printf("agerc     : %s base=%08X size=%08X sections=%zu imports=%zu resources=%s sha256=%s\n",
                        ai.path.c_str(), machine.agercBase(), ai.size_of_image, ai.sections.size(),
                        ai.imports.size(), ai.resource_rva ? "yes" : "no", ai.sha256.c_str());
        }
    } else if (verbose_output) {
        std::printf("agerc     : （没有 AGERC 镜像：--agerc 未给，且 --game-dir 下没有 AGERC.DLL）"
                    "⇒ LoadLibrary(\"AGERC.DLL\") 返回 0 + 错误 126\n");
    }
    // ★ 默认档启动回声 = **一行**（`REQ-01M7AG06MXS6PK2B53B29QM76Z`）：exe / image base / entry /
    //   sha256 与 agerc base / sha256 都在这一行里，机器可读字段一个不丢；⛔ 不是"少打就少信息"。
    if (!verbose_output) {
        std::printf("image      : %s base=%08X entry=%08X sections=%zu imports=%zu sha256=%s%s",
                    exe_path.c_str(), machine.image().image_base, machine.image().entry_ea,
                    machine.image().sections.size(), machine.image().imports.size(),
                    machine.image().sha256.c_str(), lst_sha_ok ? " lst=ok" : "");
        if (machine.agercModuleLoaded()) {
            const lift::PeImage& ai = machine.agercImage();
            std::printf(" | agerc=%s base=%08X size=%08X sha256=%s", ai.path.c_str(), machine.agercBase(),
                        ai.size_of_image, ai.sha256.c_str());
        } else {
            std::printf(" | agerc=none（LoadLibrary(\"AGERC.DLL\") 返回 0 + 错误 126）");
        }
        std::printf("\n");
    }

    if (report) {
        const bool json = std::getenv("LIFT_REPORT_JSON") != nullptr;
        if (json) {
            machine.reportCoverageJson(stdout);
        } else {
            machine.reportBaseGaps(stdout);
        }
        return 0;
    }

    // 帧导出 / 帧数上限是**宿主诊断配置**（不进 guest 语义）；只有 D3D9 Present 消费。
    machine.cpu().shim.frame_dump = frame_dump;
    machine.cpu().shim.max_frames = max_frames;
    if (!frame_dump.empty()) {
        std::printf("frame-dump: %s（每次 Present 导出 BMP；{frame} = 4 位帧号）\n", frame_dump.c_str());
    }
    if (max_frames != 0) std::printf("max-frames: %u（D3D9 Present 到达后停）\n", max_frames);

    // ---- 显示 / 光栅后端：**GPU 是唯一的光栅实现**；headless 也走同一个隐藏窗口 ----
    // ★ 后端在参数解析之后、执行源定位之前就建好了（见上面）。这里只做"接到 Machine 上"。
    if (display == nullptr) {
        // 只有 `--report` / `--smoke` 会走到这里（它们不需要光栅化）。
        std::fprintf(stderr, "emulator-lift: 内部错误：没有创建 GPU 光栅化后端（--report/--smoke 之外）\n");
        return 2;
    }
    machine.setGpuRaster(display->gpuRaster());
    std::printf("raster    : %s%s\n", display->rasterName(),
                present_mode == "headless"
                    ? "（headless 离屏；**隐藏**窗口，不出现可见窗口）"
                    : "");

    // ★ `present :` 行 **headless 也打**：headless 用的是同一个 **隐藏**窗口（`SDL_WINDOW_HIDDEN`），
    //   `window=hidden` 就是这件事的判据。值取自 `windowHidden()`（读 `SDL_GetWindowFlags`）/
    //   实际 driver，⛔ 不写死。守卫：`tests/gpu_batch.cpp` 的 `gpu-batch/headless-hidden-window`
    //   （CTest `gpu-batch-barrier`，子进程真跑 `--present headless`）。
    std::printf("present   : %s\n", display->describe().c_str());
    // ★ `--cursor-warp` 的**有效状态**一行（缺省关）：`on` / `off` / `on-but-hidden` / `unsupported`。
    //   与 `describe()` 里的 `cursor-warp=on|off`（sink 自己的开关）分工不同 —— 这一行回答
    //   "这轮真的会不会挪宿主光标"（含"开关开着但窗口是隐藏的"这种静默失效）。
    //   ★ 判据与四个状态名只有一份（`runtime/present_flags.h`），与 AOT 臂逐字相同。
    {
        const lift::CursorWarpState st = lift::cursorWarpState(
            cursor_warp, lift::hostCursorPlatformSupported(), display->windowHidden());
        std::printf("cursor-warp: %s\n", lift::cursorWarpStateName(st));
    }

    bool present_window_active = false;
    if (present_mode != "headless") {
        if (!display->isGpuRenderer()) {
            std::printf("present   : 注意：driver=%s 是 GPU API 路径，但不是 SDL_GPU（gpu driver）\n",
                        display->rendererName());
        }
        // ★ P3：把 SDL 事件源接到 WindowManager。GetMessageA 在空队列时会调用它
        //   （SDL_PollEvent → SDL_WaitEventTimeout(5ms)），把键鼠/关窗翻译成 guest 消息。
        lift::SdlWindow* sdl_raw = display.get();
        machine.presenter().setSink(std::move(display));
        sdl_raw->attachWindowManager(machine.cpu().windows);
        // ★ 交互配方驱动器也要在这里推进：guest 可能阻塞在 `GetMessageA`（等消息）——那时
        //   `Machine::run` 的块边界循环不在跑，只有宿主事件泵会被调用。
        machine.cpu().windows->setHostEventPump([sdl_raw, &machine](lift::X86& cpu) {
            sdl_raw->pumpHostEvents(5);
            machine.tickInteraction(cpu);
        });
        present_window_active = true;
    }

    lift::RunOptions opt;
    opt.until_ea = until_ea;
    opt.stop_on_first_shim = until_shim;
    opt.max_insns = bench ? bench : max_insns;
    opt.time_limit_sec = time_limit_sec;
    opt.trace_shims = trace_shims;
    opt.trace_blocks = trace_blocks;
    opt.real_time = real_time || present_window_active;
    opt.trace_inputs = trace_inputs || trace_shims;
    opt.profile = profile;
    opt.profile_top = profile_top;
    // ★ 这里**不打** "real-time ⇒ A4 的逐字节断言自动跳过" 那一行：A4 是 guest 执行 trace 的
    //   **哈希可复现**（`tests/boot_trace.cpp`，headless + 虚拟时钟下仍成立），不是渲染像素固定复现；
    //   正常游玩（window 自动 real-time）时那行没有任何信息量。真要用 real-time trace 的人看
    //   trace 输入清单里的 `clock=REAL-TIME` 字段即可。⛔ 不要再加回来。
    if (bench && present_window_active) {
        std::fprintf(stderr,
                     "emulator-lift: ⚠ --bench + 窗口呈现：MIPS 含 SDL 上传/呈现开销，不是解释器绝对吞吐\n");
    }

    // ★ 交互 trace 随 `--interaction-graph` 一并退役（2026-11-16，`REQ-01M7HQ57XV66FZQA5ECGHWD71N`）：
    //   它唯一的消费者就是那条入口。
    const std::uint64_t t0 = SDL_GetTicksNS();
    const lift::RunResult r = machine.run(opt);
    const std::uint64_t t1 = SDL_GetTicksNS();
    if (present_window_active) machine.cpu().windows->clearHostEventPump();

    machine.dumpRunSummary(r, stdout, verbose_output ? lift::Machine::SummaryMode::Verbose
                                                     : lift::Machine::SummaryMode::Normal);
    // ★ `dumpFailures=`：`--dump-frame` **每帧写文件失败的次数**（宿主诊断账）。
    //   只在这轮**要求了**帧导出时打；`0` = 一次都没失败。
    //   ⛔ 没有这行 = 本轮没要求帧导出，**不是** "0 次失败"（与 `present-tail` 同一口径）。
    //   ★ 为什么在 `dumpRunSummary` 之后由 main 打：这个计数属于 presenter（`present()` 里写），
    //     而 run summary 的字段格式由 `machine.cpp` 拥有 —— 本单不改 `machine.cpp`（并发编辑协议）。
    if (!frame_dump.empty()) {
        std::printf("dumpFailures=%llu\n", (unsigned long long)machine.presenter().dumpFailures());
    }
    if (profile) machine.dumpProfile(stdout, profile_top);
    if (bench) {
        const double secs = static_cast<double>(t1 - t0) / 1e9;
        std::printf("bench     : %llu insns in %.3f s = %.2f MIPS\n", (unsigned long long)r.insns, secs,
                    secs > 0 ? static_cast<double>(r.insns) / secs / 1e6 : 0.0);
    }
    // ★ 基座缺口（A5）：明细是**静态集合**（只随代码变），默认档只打一行条数；
    //   明细（含 `  缺口 module!api —— 理由` 的理由文本）留在 `--report` / Verbose
    //   （`--report` 的路径在上面已经 return）。⛔ 理由文本不删，只是不再每次 run 都刷 14 行。
    if (verbose_output) {
        machine.reportBaseGaps(stdout);
    } else {
        std::printf("gap       : %zu 条（--report 看明细）\n", machine.baseGapCount());
    }
    // ★ 单令牌不变式的机械判据（guest_token.h）：summary 的 `token :` 行已经先打印，
    //   这里只把"违规 > 0"变成退出码（测试子进程用它判红）。⛔ 不是"跑完没崩就算对"：
    //   并发改宿主容器**可能**这次没炸，但违规计数一定会留下证据。
    if (assert_guest_token) {
        const lift::GuestToken& token = lift::guestToken();
        // ★ 第二条判据（REQ-01M76SXW0Y3R8QX1F94AB85M9H）：`WindowManager` 是**主线程独占宿主
        //   对象**（M0 口径），⛔ 不在令牌容器清单里 ⇒ 令牌计数抓不到它。它有自己的账：
        //   非 owner 线程碰任何 wm 入口 ⇒ `offThread() > 0`。真 feeder + 输入脚本的跑法
        //   （`tests/option_roundtrip.cpp`）因此能端到端抓到"wm 越界"这一类。
        const unsigned long long wm_off = machine.windowOffThread();
        if (token.violations() != 0 || token.maxConcurrent() > 1 || wm_off != 0) {
            std::fprintf(stderr,
                         "emulator-lift: --assert-guest-token FAIL：violations=%llu max-concurrent=%u first=%s | "
                         "window off-thread=%llu first=%s\n",
                         static_cast<unsigned long long>(token.violations()), token.maxConcurrent(),
                         token.firstViolationSite() ? token.firstViolationSite() : "-", wm_off,
                         machine.firstWindowOffThreadSite() ? machine.firstWindowOffThreadSite() : "-");
            return 4;
        }
    }
    // ★ 交互任务的失败：**非 0 退出**（5，可与 2 = 未实现缺口区分）+ stderr 打 detail。
    //   ⛔ 不许 warning-only、⛔ 不许当 0 点继续跑。
    //   ★ 2026-11-16：`--input-script` 整族退役（0 点 / 效果级判据 / marker 超时随入口删除），
    //     `--interaction-graph` 更早已退役 ⇒ 本进程的 CLI 路径上这个停因**没有生产者**了
    //     （`Machine::run` 的直接消费者 = pure CTest / 宿主内嵌）。保留这条兜底是因为
    //     "诊断面不能空"：⛔ 不静默 0 —— 哪天出现新生产者，它当场就有一个响亮的出口。
    if (r.reason == lift::StopReason::InteractionFailed) {
        std::fprintf(stderr, "[interaction] %s\n", r.detail.c_str());
        return 5;
    }
    // ★ 兜底退出码抽到 `lift::exitCodeForRun`（纯函数，tests/mechanism.cpp 直接断言）：
    //   上面几道消费者闸门（token / 时间线）**优先**，它们没拦下才落到这里。
    // ★ guest 自己 `ExitProcess(n)` / `TerminateProcess(_, n)`（REQ-01M77CMJGJ9EJVNPFS2TKMMHD1）：
    //   n != 0 ⇒ 宿主退出码非 0（2，与 decode-error 同档）；n == 0 保持 0。
    //   位置在**所有消费者闸门之后**（4/5/3 那些分流不动）；「run 本身失败 ⇒ 2」也先判，
    //   于是这条只在"run 正常停下"时才有机会改变结果。
    //   ⛔ 不把 guest 的任意 n 直接当宿主退出码（见 `exitCodeForGuestExit` 的注释）。
    if (!r.ok()) return lift::exitCodeForRun(r);
    return lift::exitCodeForGuestExit(machine.guestExitCode());
}
