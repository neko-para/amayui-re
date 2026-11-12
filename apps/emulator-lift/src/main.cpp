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
#include "runtime/d3d9_pipeline.h"  // GPU 覆盖账本（--gpu-strict 的机械判据）
#include "runtime/font.h"
#include "runtime/guest_token.h"  // --assert-guest-token（单令牌违规计数）
#include "runtime/host_encoding.h"
#include "runtime/log.h"
#include "runtime/machine.h"
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

// ★ `--anchor-script <id>` 的 id 解析：十进制或 `0x` 十六进制，**0..0xFFFFFFFF**（32 位原值）。
//   ⛔ 不用 `std::strtoull`：它接受 `-1`（回绕成 ULLONG_MAX）与溢出值，而那是"参数非法"（exit 2），
//   不是"目标脚本 id = 2^64-1"。⛔ 也不接受其它进制（`08` 之类不该被当八进制）。
bool parseAnchorScriptId(const char* text, std::uint64_t* out) {
    if (text == nullptr || *text == '\0') return false;
    const char* p = text;
    int base = 10;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
        p += 2;
    }
    if (*p == '\0') return false;
    std::uint64_t v = 0;
    for (; *p != '\0'; ++p) {
        int d = -1;
        if (*p >= '0' && *p <= '9') {
            d = *p - '0';
        } else if (base == 16 && *p >= 'a' && *p <= 'f') {
            d = *p - 'a' + 10;
        } else if (base == 16 && *p >= 'A' && *p <= 'F') {
            d = *p - 'A' + 10;
        }
        if (d < 0 || d >= base) return false;
        v = v * static_cast<std::uint64_t>(base) + static_cast<std::uint64_t>(d);
        if (v > 0xFFFFFFFFull) return false;  // id 是 32 位原值（含 ≥0x80000000 的"负 id"形态）
    }
    *out = v;
    return true;
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
        "  --real-time         用宿主时钟（此时 A4 的逐字节断言自动跳过并明说）\n"
        "  --skip-logo         把引擎自己的 show-logo 标志置 0（根脚本 0x130 因而不 call LOGO；\n"
        "                      仍走完整 boot 到 TITLE，⛔ 不是直接跳到 TITLE 的宿主捷径）\n"
        "  --accept-recovery-dialog  恢复引擎真实的「前回正常終了しませんでした」判定\n"
        "                      （**默认绕过**：等价于「上次正常结束」= marker 不存在；传它才会走到\n"
        "                      恢复框。⛔ 不是替用户答 NO；默认档 MessageBoxA **一次都不会被调**）\n"
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
        "  --input-script <path>  指令式输入时间线（`at frame N` / `at insn N` / `at guest_ms T` / `at wall_ms T` / at marker <名字> / after / wait；\n"
        "                     动作 = mouse_move / mouse_down / mouse_up / click / key_down / key_up / profile_begin / profile_end / quit）。\n"
        "                     段标记（各独占一行、**必须成对**，且需 --anchor-script）：\n"
        "                       before-anchor  之后 = **绝对**段：按声明值执行（不随 0 点重定基、闩锁前也执行）；\n"
        "                       after-anchor   之后 = **相对**段：参与 0 点重定基、闩锁前被抑制；\n"
        "                       ⛔ 缺 after-anchor = 解析期错误（不猜\"绝对到文件尾\"还是\"绝对止于闩锁\"）；\n"
        "                       ⛔ 绝对段内不许 profile_begin / profile_end（窗口 profiler 的 0 点语义与锚点冲突：\n"
        "                          0 点之前的时刻与 0 点之后不是同一把尺子）\n"
        "                     注入**不需要窗口焦点、也不移动真实光标**；格式见 README §4.2c\n"
        "  --input-marker-timeout-ms <ms>  `at marker <名字>` 等不到的超时（**宿主墙钟** ms，缺省 30000；0 = 不设限）；\n"
        "                             到点 ⇒ InteractionFailed + `marker-timeout:` detail + exit 5（⛔ 不静默继续）\n"
        "  --anchor-script <id>  时间线的 **0 点** = 首次**观察到**进入引擎脚本 <id>（十进制 / `0x` 十六进制；\n"
        "                     0 是根脚本的**合法** id，⛔ 不是\"关闭\"）。0 点作用于 frame / insn / guest_ms / wall_ms；\n"
        "                     与 --interaction-graph 互斥；跑完仍未观察到 ⇒ InteractionFailed + `origin-missed:` + exit 5\n"
        "  --input-stall-ms <ms>      帧号停住多久之后把剩余 frame 条件折成 wall_ms（缺省 10000；0 = 关闭兜底）\n"
        "  --input-stall-boot-ms <ms> 还没出现第一帧（boot）时的宽限（缺省 30000；⛔ boot 不算停滞）\n"
        "  --interaction-graph <path>  识别驱动的交互任务（节点图：when → action → next）。与 --input-script **并存**；\n"
        "                     格式与三种失败见 docs/03-emulator/interaction-model.md。⛔ 不写帧号\n"
        "  --interaction-trace <path>  交互任务的 trace（一行一事件；确定性字段，可逐行 diff）\n"
        "  --interaction-fail-dump <path>  失败时补写「最后呈现的那一帧」BMP（缺省 <graph>.fail.bmp）\n"
        "  --input-max-hold <ms>      自动 hold 的上限（guest ms，缺省 1000；0 = 不设限）：`click` / 显式 up\n"
        "                             等 guest 采样，到上限仍未采样 ⇒ 抬起 + 告警（⛔ 不挂死）；见 README §4.2c\n"
        "  --expect-change-after <list>  时间线的**效果级判据**（缺省关）：动作应用之后必须在窗口内看到\n"
        "                             画面变化；list = click[,key,mouse]。三种失败（没变 / 没采样 /\n"
        "                             跑完未判定）一律 stderr `[expect-change]` + exit 5；见 README §4.2c\n"
        "  --expect-change-window <samples>  观测窗长度（**采样数**，一次 Present = 一个样本；缺省 30）。\n"
        "                             ⛔ 不是毫秒：`--present window` 是 real-time 时钟，毫秒窗口不可复现\n"
        "  --audio off|sdl|virtual  音频后端（缺省 sdl = 真设备；virtual = 确定性设备时钟；off = silent）\n"
        "  --audio-dump <path>  把混音结果写成 WAV（44.1k/16/2；与 sdl 同时用 = tee）\n"
        "  --sound              允许出声（缺省静音：设备/混音/喂料/guest DS 调用照跑，只在交付前置 0）\n"
        "  --raster <mode>    auto（缺省）/ cpu / gpu：D3D9 fixed-function 光栅化后端\n"
        "                      auto = headless⇒cpu、window⇒gpu；window 下 GPU 初始化失败 ⇒ exit 2\n"
        "  --gpu-strict       GPU 启用时，运行结束若仍有 Draw/ sprite 回退 CPU ⇒ 打印错误并 exit 3\n"
        "                      （先打印 summary；需要 GPU 光栅化模式，--raster cpu 会被拒绝）\n"
        "  --assert-guest-token  运行结束若单令牌违规计数 > 0（或有 >1 个令牌持有者）⇒ exit 4\n"
        "                      （判据 = summary 的 `token : violations=0`；见 runtime/guest_token.h）\n"
        "                      或窗口状态被非主线程访问（summary 的 `window : off-thread=0`）⇒ exit 4\n"
        "                      （WindowManager 是主线程独占宿主对象，见 runtime/host_window.h）\n"
        "  --raster-sprites 0|1      D3DXSprite 是否也走 GPU（缺省 1；A/B 用）\n"
        "  --font <path>       宿主字体（缺省：CMake 注入的 Amayui CN；调试覆盖）\n"
        "  --native-overrides <list>  逗号分隔的 guest 函数名 / 段级项名（如 sub_414AC0）；all = 已验证项；\n"
        "                             -名字 = 排除该项；也可显式叠加调试项（缺省关）\n"
        "  --baseline-lift <N> 宿主基线补偿像素（缺省 2；0 = 关；★ 宿主视觉策略，不是引擎语义）\n"
        "  --font-engine-name  字体 A/B 对照档：上报引擎字面名（字形仍来自宿主 Amayui CN）\n"
        "  --smoke             只验证宿主层可链接（不建窗口、不需要游戏）\n");
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
    std::string input_script_path;
    // ★ 识别驱动的交互任务（`--interaction-graph`）：见 docs/03-emulator/interaction-model.md。
    std::string interaction_graph_path;
    std::string interaction_trace_path;
    std::string interaction_fail_dump;
    // 图是否成功加载：`interactionGraph().active()` 在 `abandon()`/清空后会变假，而"任务没走完"
    // 的判定要的是"本次运行**本来**有个任务"。
    bool interaction_graph_loaded = false;
    // ★ 帧停滞兜底（README §4.2c）：帧不推进时把剩余 frame 触发条件折算成 wall_ms。
    //   0 = 关闭（显式选择"宁可挂死也不提前"）。
    std::uint64_t input_stall_ms = 10000;
    // 还没出现第一帧时的宽限（boot 期间没有 Present 不算停滞）。
    std::uint64_t input_stall_boot_ms = 30000;
    // ★ 自动 hold 的上限（README §4.2c）：`click` / 显式 up 等 guest 采样，等不到就到这里抬起 + 告警。
    //   0 = 不设限（显式选择"宁可挂死"）。
    std::uint64_t input_max_hold_ms = 1000;
    // ★ marker 超时（`at marker <名字>` 等不到）：**宿主墙钟** ms；0 = 不设限。
    //   到点 ⇒ `InteractionFailed` + `marker-timeout:` detail + exit 5（⛔ 不静默继续）。
    std::uint64_t input_marker_timeout_ms = 30000;
    // ★ 时间线的 **0 点**（`--anchor-script <id>`，见 `runtime/input_script.h` 的 `TimelineOrigin`）。
    //   ★ 启用判据是 `anchor_script_set`，⛔ 不是 `id != 0` —— `0` 是**根脚本的合法 id**
    //     （`engine_marker.cpp` 明说），拿它当"关闭"哨兵会让"锚根脚本"变成"没给"。
    bool anchor_script_set = false;
    std::uint32_t anchor_script_id = 0;
    // ★ 时间线的效果级判据（README §4.2c）：`--expect-change-after` 的原始写法 + 窗口长度（采样数）。
    //   缺省关（打开会改变既有脚本的退出码）。窗口缺省 30 个采样（60 fps 下 ≈0.5 s）。
    std::string expect_change_after;
    std::uint64_t expect_change_window = 30;
    bool expect_change_window_set = false;
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
    std::string raster_mode = "auto";
    bool raster_sprites = true;
    bool gpu_strict = false;
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
        } else if (std::strcmp(a, "--input-script") == 0 && i + 1 < arg_count) {
            input_script_path = args[++i];
            if (input_script_path.empty()) {
                std::fprintf(stderr, "--input-script 需要一个非空路径\n");
                return 2;
            }
        } else if (std::strcmp(a, "--interaction-graph") == 0 && i + 1 < arg_count) {
            interaction_graph_path = args[++i];
            if (interaction_graph_path.empty()) {
                std::fprintf(stderr, "--interaction-graph 需要一个非空路径\n");
                return 2;
            }
        } else if (std::strcmp(a, "--interaction-trace") == 0 && i + 1 < arg_count) {
            interaction_trace_path = args[++i];
        } else if (std::strcmp(a, "--interaction-fail-dump") == 0 && i + 1 < arg_count) {
            interaction_fail_dump = args[++i];
        } else if (std::strcmp(a, "--input-stall-ms") == 0 && i + 1 < arg_count) {
            // 帧停滞兜底阈值（0 = 关闭）。见 README §4.2c。
            const char* text = args[++i].c_str();
            char* end = nullptr;
            const unsigned long long v = std::strtoull(text, &end, 10);
            if (end == text || *end != '\0') {
                std::fprintf(stderr, "--input-stall-ms 需要非负整数（收到 %s）\n", text);
                return 2;
            }
            input_stall_ms = static_cast<std::uint64_t>(v);
        } else if (std::strcmp(a, "--input-stall-boot-ms") == 0 && i + 1 < arg_count) {
            const char* text = args[++i].c_str();
            char* end = nullptr;
            const unsigned long long v = std::strtoull(text, &end, 10);
            if (end == text || *end != '\0') {
                std::fprintf(stderr, "--input-stall-boot-ms 需要非负整数（收到 %s）\n", text);
                return 2;
            }
            input_stall_boot_ms = static_cast<std::uint64_t>(v);
        } else if (std::strcmp(a, "--input-max-hold") == 0 && i + 1 < arg_count) {
            // 自动 hold 的上限（guest ms；0 = 不设限）。见 README §4.2c。
            const char* text = args[++i].c_str();
            char* end = nullptr;
            const unsigned long long v = std::strtoull(text, &end, 10);
            if (end == text || *end != '\0') {
                std::fprintf(stderr, "--input-max-hold 需要非负整数（收到 %s）\n", text);
                return 2;
            }
            input_max_hold_ms = static_cast<std::uint64_t>(v);
        } else if (std::strcmp(a, "--input-marker-timeout-ms") == 0 && i + 1 < arg_count) {
            // `at marker <名字>` 等不到的超时（宿主墙钟 ms；0 = 不设限）。见 README §4.2c。
            // ★ 只收十进制非负整数：`strtoull` 会把 `-1` 回绕成 ULLONG_MAX（那是非法参数，不是"不设限"）。
            const char* text = args[++i].c_str();
            bool digits = text[0] != '\0';
            for (const char* p = text; *p != '\0'; ++p) {
                if (*p < '0' || *p > '9') {
                    digits = false;
                    break;
                }
            }
            char* end = nullptr;
            errno = 0;
            const unsigned long long v = digits ? std::strtoull(text, &end, 10) : 0ull;
            if (!digits || end == text || *end != '\0' || errno == ERANGE) {
                std::fprintf(stderr, "--input-marker-timeout-ms 需要非负整数（收到 %s）\n", text);
                return 2;
            }
            input_marker_timeout_ms = static_cast<std::uint64_t>(v);
        } else if (std::strcmp(a, "--anchor-script") == 0 && i + 1 < arg_count) {
            // 时间线 0 点 = 首次**观察到**进入脚本 <id>（语义见 runtime/input_script.h 的 TimelineOrigin）。
            const char* text = args[++i].c_str();
            std::uint64_t v = 0;
            if (!parseAnchorScriptId(text, &v)) {
                std::fprintf(stderr,
                             "--anchor-script 需要 0..0xFFFFFFFF 的整数（十进制或 0x 十六进制；收到 %s）\n",
                             text);
                return 2;
            }
            anchor_script_set = true;
            anchor_script_id = static_cast<std::uint32_t>(v);
        } else if (std::strcmp(a, "--expect-change-after") == 0 && i + 1 < arg_count) {
            // 效果级判据：动作之后必须观测到画面变化（见 README §4.2c）。
            expect_change_after = args[++i];
            if (expect_change_after.empty()) {
                std::fprintf(stderr, "--expect-change-after 需要一个非空列表（click[,key,mouse]）\n");
                return 2;
            }
        } else if (std::strcmp(a, "--expect-change-window") == 0 && i + 1 < arg_count) {
            // ★ 只收**十进制正整数**：`std::strtoull` 会接受 `-1`（回绕成 ULLONG_MAX）与溢出值，
            //   而那是"参数非法"（必须 exit 2），不是"窗口 2^64-1"。⇒ 先逐字符查数字，再查范围。
            const char* text = args[++i].c_str();
            expect_change_window_set = true;
            bool digits = text[0] != '\0';
            for (const char* p = text; *p != '\0'; ++p) {
                if (*p < '0' || *p > '9') {
                    digits = false;
                    break;
                }
            }
            char* end = nullptr;
            errno = 0;
            const unsigned long long v = digits ? std::strtoull(text, &end, 10) : 0ull;
            if (!digits || end == text || *end != '\0' || errno == ERANGE || v == 0) {
                std::fprintf(stderr,
                             "--expect-change-window 需要正整数（单位 = 采样数，一次 Present = 一个样本；"
                             "收到 %s）\n",
                             text);
                return 2;
            }
            expect_change_window = static_cast<std::uint64_t>(v);
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
        } else if (std::strcmp(a, "--raster") == 0 && i + 1 < arg_count) {
            raster_mode = args[++i];
        } else if (std::strcmp(a, "--raster-sprites") == 0 && i + 1 < arg_count) {
            raster_sprites = std::atoi(args[++i].c_str()) != 0;
        } else if (std::strcmp(a, "--gpu-strict") == 0) {
            gpu_strict = true;
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

    // ---- ★ `--anchor-script` 的**解析期**校验（互斥 / 依赖；⛔ 不跑一半就退化成"0 点从 run 起算"）----
    //   判据的**唯一实现**在 `runtime/input_script.cpp`（`validateOriginCli`）：那里也是 pure CTest
    //   断言"与 --interaction-graph 同给 ⇒ 解析期报错"的入口（进程退出码在 CTest 里不可达）。
    {
        const std::string why =
            lift::validateOriginCli(anchor_script_set, !input_script_path.empty(), !interaction_graph_path.empty());
        if (!why.empty()) {
            std::fprintf(stderr, "emulator-lift: %s\n", why.c_str());
            return 2;
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
    // ---- 呈现后端参数校验（缺省 headless；窗口参数给了就必须显式 --present window/auto）----
    if (present_mode != "headless" && present_mode != "window" && present_mode != "auto") {
        std::fprintf(stderr, "--present 只能是 headless / window / auto（收到 %s）\n", present_mode.c_str());
        return 2;
    }
    const bool present_window_flag =
        present_driver_set || present_vsync_set || present_hidden || present_screenshot_set || overlay;
    if (present_screenshot_frame != 0 && present_screenshot.empty()) {
        std::fprintf(stderr, "--present-screenshot-frame 需要同时给 --present-screenshot <path>\n");
        return 2;
    }
    if (present_mode == "headless" && present_window_flag) {
        std::fprintf(stderr, "--present headless 不接受窗口参数（--present-driver / --present-vsync / "
                             "--present-hidden / --present-screenshot / --overlay）\n");
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
    if (raster_mode != "auto" && raster_mode != "cpu" && raster_mode != "gpu") {
        std::fprintf(stderr, "--raster 只能是 auto / cpu / gpu（收到 %s）\n", raster_mode.c_str());
        return 2;
    }
    // ★ auto（缺省）：headless ⇒ CPU（保持 headless 确定性 / CTest 口径）；
    //   window / auto ⇒ GPU。显式 cpu 是参考后端；显式 gpu 仍可配 headless 离屏。
    const bool raster_wants_gpu = raster_mode == "gpu" || (raster_mode == "auto" && present_mode != "headless");
    if (raster_mode == "auto") {
        std::printf("raster    : auto ⇒ %s（present=%s）\n", raster_wants_gpu ? "gpu" : "cpu",
                    present_mode.c_str());
    }
    if (gpu_strict) {
        if (raster_mode == "cpu") {
            std::fprintf(stderr, "--gpu-strict 与 --raster cpu 矛盾（CPU 是参考后端，没有 GPU 覆盖可判）\n");
            return 2;
        }
        if (!raster_wants_gpu) {
            std::fprintf(stderr, "--gpu-strict 需要 GPU 光栅化（--present window，或 --raster gpu）\n");
            return 2;
        }
        if (raster_mode == "auto" && present_mode != "window") {
            std::fprintf(stderr, "--gpu-strict + --raster auto 需要 --present window（否则 auto 会选 CPU）\n");
            return 2;
        }
        if (!raster_sprites) {
            // sprite 是覆盖的一部分；显式关掉然后再声称 strict 通过 = 自欺。
            std::fprintf(stderr,
                         "--gpu-strict 与 --raster-sprites 0 矛盾（sprite 全部走 CPU，GPU 覆盖不完整）\n");
            return 2;
        }
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
        std::printf("font      : %s（baseline-lift=%d%s）\n",
                    lift::fontConfig().regular_path.empty() ? "(未配置 ⇒ 文本 API 会明确失败)"
                                                           : lift::fontConfig().regular_path.c_str(),
                    lift::fontConfig().baseline_lift,
                    lift::fontConfig().use_engine_name ? "；A/B 对照档（只影响上报族名）" : "");
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
        std::printf("game-dir  : %s\n", game_dir.c_str());
    } else {
        std::fprintf(stderr,
                     "emulator-lift: ⚠ 没有 --game-dir（也没有 AMAYUI_GAME_DIR）：文件与路径族会**响亮失败**\n"
                     "  （S6：guest 路径只映射到 --game-dir；⛔ 不回退到任意宿主目录）\n");
    }
    if (!appdata_dir.empty()) {
        machine.vfs().setAppDataDir(appdata_dir);
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
        std::printf("sha256 ok  : %s（与 .lst 头部一致）\n", pe_sha.c_str());
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
        std::printf("native-ovr: %zu 项 + %zu 段级（--native-overrides=%s）\n", machine.overrides().size(),
                    machine.blockOverrides().size(), native_overrides.c_str());
        for (std::size_t i = 0; i < machine.overrides().size(); ++i) {
            const lift::NativeOverride* e = machine.overrides().at(i);
            const lift::KnownNative* k = lift::findKnownNative(e->name);
            std::printf("  %-12s @ %08X %s(%uB)%s\n", e->name.c_str(), e->ea, lift::guestConvName(e->conv),
                        e->arg_bytes, (k != nullptr && k->debug) ? " [debug]" : "");
            if (k != nullptr && k->debug) {
                std::fprintf(stderr,
                             "emulator-lift: ⚠ debug native override %s：已知未收口，仅用于复现/调试；"
                             "不要把它当作 A4 逐字节基线\n",
                             e->name.c_str());
            }
        }
        for (std::size_t i = 0; i < machine.blockOverrides().size(); ++i) {
            const lift::NativeBlockOverride* e = machine.blockOverrides().at(i);
            std::printf("  %-12s @ %08X..%08X 段级（函数中段；跳到出口）\n", e->name.c_str(), e->entry_ea,
                        e->resume_ea);
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
    std::printf("image      : %s\n  base=%08X entry=%08X sections=%zu imports=%zu sha256=%s\n", exe_path.c_str(),
                machine.image().image_base, machine.image().entry_ea, machine.image().sections.size(),
                machine.image().imports.size(), machine.image().sha256.c_str());
    for (const lift::PeSectionInfo& s : machine.image().sections) {
        std::printf("  %-8s va=%08X vsize=%8X raw=%8X flags=%08X\n", s.name, s.va, s.vsize, s.raw_size,
                    s.characteristics);
    }

    if (!machine.prepare(0, &err)) {
        std::fprintf(stderr, "emulator-lift: 装配 guest 进程结构失败：%s\n", err.c_str());
        return 1;
    }
    machine.setSkipLogo(skip_logo);
    // ★ 默认**绕过**恢复判定（= "marker 不存在" = 上次正常结束）；传 flag 才走引擎真实判定。
    machine.setAcceptRecoveryDialog(accept_recovery_dialog);

    // ★ 0 点的**配置**必须先于脚本解析：`before-anchor` / `after-anchor` 的解析期判据要问
    //   "有没有 `--anchor-script`"（没开却写段标记 ⇒ 解析期报错）。配置跨 `clear()` 存活
    //   ⇒ 放在 `loadInputScript` 之前不会丢（`REQ-01M710E6PV4PPHECPPCCTRF5RA`）。
    if (anchor_script_set) machine.inputScript().setOriginScript(anchor_script_id);
    // ---- 指令式输入时间线（`--input-script`）：启动前解析，失败立刻拒绝（⛔ 不跑一半）----
    if (!input_script_path.empty()) {
        std::string script_err;
        if (!machine.loadInputScript(input_script_path, &script_err)) {
            std::fprintf(stderr, "emulator-lift: --input-script 解析失败\n  %s\n", script_err.c_str());
            return 2;
        }
    }
    // ---- 识别驱动的交互任务（`--interaction-graph`）：同样启动前解析，失败立刻拒绝 ----
    //   ★ 与时间线并存（真源 §7 第 3 期才把时间线并入）；两条通道都写同一份 WindowManager 状态。
    if (!interaction_graph_path.empty()) {
        std::string graph_err;
        if (!machine.loadInteractionGraph(interaction_graph_path, &graph_err)) {
            std::fprintf(stderr, "emulator-lift: --interaction-graph 解析失败\n  %s\n", graph_err.c_str());
            return 2;
        }
        interaction_graph_loaded = true;
    }
    // ★ 输入旋钮对**两条通道**同时生效（时间线 + 交互图的 actuator）。放在两处加载之后：
    //   图的动作是追加进它自己的 actuator 的，用户传的 `--input-max-hold` / `--input-stall-ms`
    //   必须也落在那里 —— 否则那几个 flag 对图静默无效。
    //   自动 hold 的上限还要**小于**节点 timeout：否则上一条 click 的抬起会一直挡住下一条追加。
    if (!input_script_path.empty() || interaction_graph_loaded) {
        machine.setInputStallMs(input_stall_ms);
        machine.setInputStallBootGraceMs(input_stall_boot_ms);
        machine.setInputMaxHoldMs(input_max_hold_ms);
    }
    // ★ marker 只是**时间线**的事件源（交互图的 actuator 生成的是 `at frame`，没有 marker 动作）。
    if (!input_script_path.empty()) machine.setInputMarkerTimeoutMs(input_marker_timeout_ms);
    // ★ 时间线的 0 点（`--anchor-script <id>`）：配置 + 启动日志 + "不可复现"的警告。
    //   ★ 配置**跨 run / clear() 存活**（`setOriginScript` 只写配置；闩锁状态每轮重置）——
    //     所以必须在这里、`run()` 之前设。
    if (anchor_script_set) {
        const std::string why = lift::validateOriginScript(true, machine.inputScript().actionCount());
        if (!why.empty()) {
            std::fprintf(stderr, "emulator-lift: %s\n", why.c_str());
            return 2;
        }
        // ★ 配置已在解析**之前**设过（段标记的解析期判据要用它）—— 这里只做启动日志与警告。
        std::printf(
            "anchor    : 0 点 = 首次**观察到**进入脚本 0x%08X（frame / insn / guest_ms / wall_ms 全部重定基；\n"
            "            跑完仍未观察到 ⇒ stderr `[anchor] FAIL origin-missed:` + exit 5）\n",
            anchor_script_id);
        // ★ 裁决 6：`at wall_ms` **允许**，但必须打一行"该锚在 real-time 档不可复现"的警告
        //   （⛔ 不因此 exit 2：墙钟在 headless 档是确定性的，脚本本身仍然可跑）。
        bool has_wall_ms = false;
        for (const lift::InputScript::Action& act : machine.inputScript().actions()) {
            if (act.trigger.kind == lift::TriggerKind::WallMs) {
                has_wall_ms = true;
                break;
            }
        }
        if (has_wall_ms) {
            std::fprintf(stderr,
                         "emulator-lift: ⚠ 脚本里有 `at wall_ms` 条件，而 0 点来自脚本身份 ⇒ 该锚在 real-time 档"
                         "（--present window）**不可复现**（墙钟不随 guest 脚本推进；那一档请用 frame / insn）\n");
        }
    }
    if (!interaction_fail_dump.empty() && !interaction_graph_loaded) {
        std::fprintf(stderr, "--interaction-fail-dump 需要同时给 --interaction-graph\n");
        return 2;
    }
    // ★ 时间线的效果级判据（`--expect-change-after` / `--expect-change-window`）：解析 + 配置。
    //   缺省关；四条拒绝都是 exit 2（⛔ 不静默退化成"关"—— 静默失败正是本单要消灭的东西）：
    //   ① 没给 `--input-script`（判的是时间线的动作；没有脚本 = 一个动作都不会被看）；
    //   ② 与 `--interaction-graph` 同用（图有自己的 `expect` 语义 ⇒ 两套口径）；
    //   ③ 类别 / 数字写错（窗口只收十进制正整数）；
    //   ④ 脚本里**没有匹配的动作**（否则一个观测窗都开不了 ⇒ "静默绿"，与"发完不管"同类）；
    //      只给 `--expect-change-window` 而不给 `--expect-change-after` 也拒绝（它只在策略开时生效）。
    if (expect_change_window_set && expect_change_after.empty()) {
        std::fprintf(stderr,
                     "--expect-change-window 只在 --expect-change-after 开启时生效；单独给它不会看任何动作\n");
        return 2;
    }
    if (!expect_change_after.empty()) {
        if (input_script_path.empty()) {
            std::fprintf(stderr,
                         "--expect-change-after 需要同时给 --input-script（它判的是时间线动作有没有生效）\n");
            return 2;
        }
        lift::ExpectChangePolicy policy;
        std::string policy_err;
        if (!lift::parseExpectChangeActions(expect_change_after, &policy, &policy_err)) {
            std::fprintf(stderr, "emulator-lift: --expect-change-after 解析失败：%s\n", policy_err.c_str());
            return 2;
        }
        policy.window_samples = expect_change_window;
        if (!machine.configureExpectChange(policy, &policy_err)) {
            std::fprintf(stderr, "emulator-lift: --expect-change-after 配置失败：%s\n", policy_err.c_str());
            return 2;
        }
        std::printf("expect    : after=%s（窗口 %llu 个采样；失败 ⇒ stderr [expect-change] + exit 5）\n",
                    policy.text.c_str(), (unsigned long long)policy.window_samples);
    }
    // ★ guest 眼里的主模块路径（用户 2026-11-05 口径）：`<guest 根>\AGE.EXE`，
    //   打开它拿到的是**我们真正在跑的那份字节**（拆壳件），不是 `--game-dir` 下的加壳原件。
    std::printf("exe(guest): %s（VFS overlay ⇒ %s）\n", machine.vfs().overlayGuestPath().c_str(),
                machine.vfs().overlayHostPath().c_str());

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
        } else {
            const lift::PeImage& ai = machine.agercImage();
            std::printf(
                "agerc     : %s\n  base=%08X size=%08X sections=%zu imports=%zu resources=%s sha256=%s\n",
                ai.path.c_str(), machine.agercBase(), ai.size_of_image, ai.sections.size(),
                ai.imports.size(), ai.resource_rva ? "yes" : "no", ai.sha256.c_str());
        }
    } else {
        std::printf("agerc     : （没有 AGERC 镜像：--agerc 未给，且 --game-dir 下没有 AGERC.DLL）"
                    "⇒ LoadLibrary(\"AGERC.DLL\") 返回 0 + 错误 126\n");
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

    // ---- 可选显示后端（缺省 headless：不初始化 SDL 视频，行为与今天逐字节一致）----
    bool present_window_active = false;
    if (present_mode != "headless") {
        lift::SdlWindowOptions wopt;
        wopt.title = "emulator-lift";
        wopt.driver_list = present_driver;
        wopt.vsync = present_vsync != 0;
        wopt.hidden = present_hidden;
        wopt.screenshot_path = present_screenshot;
        wopt.screenshot_frame = present_screenshot_frame;
        wopt.overlay = overlay;
        wopt.raster_gpu = raster_wants_gpu;
        wopt.raster_sprites = raster_sprites;
        auto sdl = std::make_unique<lift::SdlWindow>();
        std::string werr;
        if (!sdl->init(wopt, &werr)) {
            if (present_mode == "auto") {
                std::fprintf(stderr, "emulator-lift: --present auto：窗口打开失败，回退 headless：%s\n",
                             werr.c_str());
                if (!present_screenshot.empty()) {
                    std::fprintf(stderr, "  ⚠ --present-screenshot 不会产出（headless 没有窗口像素）\n");
                }
                if (raster_wants_gpu) {
                    std::fprintf(stderr,
                                 "  ⚠ --raster auto/gpu：GPU 光栅化后端也随之不可用 ⇒ 本轮是 CPU 光栅化\n");
                }
            } else {
                std::fprintf(stderr, "emulator-lift: --present window 失败：%s\n", werr.c_str());
                return 2;
            }
        } else {
            std::printf("present   : %s\n", sdl->describe().c_str());
            if (raster_wants_gpu) std::printf("raster    : %s\n", sdl->rasterName());
            if (raster_wants_gpu && !sdl->rasterActive()) {
                // `--present window` 下 GPU 光栅化是 first-class 路径：不能静默退回 CPU。
                std::fprintf(stderr, "emulator-lift: --present window 要求 GPU 光栅化，但后端未激活（拒绝退回 CPU）\n");
                return 2;
            }
            if (!sdl->isGpuRenderer()) {
                std::printf("present   : 注意：driver=%s 是 GPU API 路径，但不是 SDL_GPU（gpu driver）\n",
                            sdl->rendererName());
            }
            // ★ P3：把 SDL 事件源接到 WindowManager。GetMessageA 在空队列时会调用它
            //   （SDL_PollEvent → SDL_WaitEventTimeout(5ms)），把键鼠/关窗翻译成 guest 消息。
            lift::SdlWindow* sdl_raw = sdl.get();
            machine.presenter().setSink(std::move(sdl));
            sdl_raw->attachWindowManager(machine.cpu().windows);
            // ★ 注入脚本也要在这里推进：guest 可能阻塞在 `GetMessageA`（等消息）——那时
            //   `Machine::run` 的块边界循环不在跑，只有宿主事件泵会被调用。
            machine.cpu().windows->setHostEventPump([sdl_raw, &machine](lift::X86& cpu) {
                sdl_raw->pumpHostEvents(5);
                machine.tickInputScript(cpu);
                // ★ 交互图也要在这里推进：理由与时间线同一条 —— guest 阻塞在 `GetMessageA` 时
                //   没有块边界，只有宿主的这个泵会被调用。
                machine.tickInteraction(cpu);
            });
            present_window_active = true;
        }
    }

    std::unique_ptr<lift::SdlWindow> raster_only;
    if (raster_mode == "gpu" && present_mode == "headless") {
        auto win = std::make_unique<lift::SdlWindow>();
        lift::SdlWindowOptions ropt;
        ropt.title = "emulator-lift raster (offscreen)";
        ropt.driver_list = present_driver;
        ropt.hidden = true;
        ropt.raster_gpu = true;
        ropt.raster_sprites = raster_sprites;
        std::string rerr;
        if (!win->init(ropt, &rerr)) {
            std::fprintf(stderr, "emulator-lift: --raster gpu（headless）失败：%s\n", rerr.c_str());
            return 2;
        }
        std::printf("raster    : %s（headless 离屏；SDL 视频已初始化）\n", win->rasterName());
        machine.setGpuRaster(win->gpuRaster());
        raster_only = std::move(win);
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
    if (opt.real_time) {
        std::fprintf(stderr,
                     "emulator-lift: ⚠ %s：宿主时钟进 guest ⇒ **A4 的逐字节可复现断言自动跳过**"
                     "（trace 输入清单里的 clock=REAL-TIME）\n",
                     present_window_active ? "窗口模式（自动 real-time）" : "--real-time");
    }
    if (bench && present_window_active) {
        std::fprintf(stderr,
                     "emulator-lift: ⚠ --bench + 窗口呈现：MIPS 含 SDL 上传/呈现开销，不是解释器绝对吞吐\n");
    }

    // ★ 交互 trace：首行必须标 `NON-DIFFABLE` 与否 ⇒ 只能在**知道呈现档位之后**开
    //   （`--present window` 是 real-time 时钟，那一档的 trace 不参与 diff）。见真源 §4。
    //   ★ 墙钟等待（图里出现过 `wait … wall_ms`）⇒ 时序不可复现：同一输入两次跑的完成时刻
    //     取决于宿主调度 ⇒ 那种图的 trace 一律标 `NON-DIFFABLE`（与窗口档同理）。
    std::FILE* interaction_trace = nullptr;
    if (!interaction_trace_path.empty()) {
        if (!machine.interactionGraph().active()) {
            std::fprintf(stderr, "--interaction-trace 需要同时给 --interaction-graph\n");
            return 2;
        }
        interaction_trace = lift::openHostFileUtf8(interaction_trace_path, "wb");
        if (interaction_trace == nullptr) {
            std::fprintf(stderr, "emulator-lift: 打不开 --interaction-trace 输出：%s\n",
                         interaction_trace_path.c_str());
            return 2;
        }
        machine.interactionGraph().setTrace(interaction_trace,
                                            !present_window_active && !machine.interactionGraph().usesWallClock());
    }

    const std::uint64_t t0 = SDL_GetTicksNS();
    const lift::RunResult r = machine.run(opt);
    if (interaction_trace != nullptr) {
        std::fclose(interaction_trace);
        interaction_trace = nullptr;
    }
    const std::uint64_t t1 = SDL_GetTicksNS();
    if (present_window_active) machine.cpu().windows->clearHostEventPump();

    machine.dumpRunSummary(r, stdout);
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
    machine.reportBaseGaps(stdout);
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
    if (gpu_strict) {
        // ★ `--gpu-strict`：给 CTest / 真实 boot 用的**机械判据**（summary 已先打印）。
        if (!r.ok()) return 2;
        lift::GpuRaster* gpu = machine.gpuRaster();
        const lift::d3d9::GpuCoverage& cov = lift::d3d9::gpuCoverage();
        const std::uint64_t fallback = cov.fallback_draws + cov.sprite_fallback_draws;
        if (gpu == nullptr || !gpu->rasterActive()) {
            std::fprintf(stderr,
                         "emulator-lift: --gpu-strict FAIL：GPU 光栅化后端没有启用（不能把 CPU 路径当成覆盖）\n");
            return 3;
        }
        if (fallback != 0) {
            std::fprintf(stderr,
                         "emulator-lift: --gpu-strict FAIL：%llu 批 Draw 回退 CPU"
                         "（draw=%llu, sprite=%llu；fallback_pixels=%llu；gpu_failures=%llu）\n",
                         (unsigned long long)fallback, (unsigned long long)cov.fallback_draws,
                         (unsigned long long)cov.sprite_fallback_draws,
                         (unsigned long long)cov.fallback_pixels,
                         (unsigned long long)cov.gpu_failures);
            for (const auto& kv : cov.fallback_reasons) {
                std::fprintf(stderr, "  [%llu×] %s\n", (unsigned long long)kv.second, kv.first.c_str());
            }
            return 3;
        }
        std::printf("gpu-strict: OK（draws=%llu gpu=%llu fallback=0 sprite=%llu/%llu）\n",
                    (unsigned long long)cov.draw_calls, (unsigned long long)cov.gpu_draws,
                    (unsigned long long)cov.sprite_gpu_draws, (unsigned long long)cov.sprite_draws);
    }
    // ★ 交互任务**没走完**就必须响亮：`Stopped` 是"实现里主动停"（`ok()` 为真，退出 0），
    //   但任务图只认自己的 `done()` —— guest 侧硬停（d3dxTrap 的"未实现方法槽" / 资源耗尽 /
    //   ExitProcess）会让运行"看起来成功"而任务其实停在半路。实测：EU-ROOM 里按 ESC ⇒
    //   `stop: stopped` + `detail: unimplemented … LockRect` + **exit 0**（静默当成成功）。
    //   ⛔ 这里不猜是哪一种停：任务没到 `task done` 就是失败（退出码与三种失败同一个 5）。
    if (interaction_graph_loaded && !machine.interactionGraph().done() &&
        r.reason != lift::StopReason::InteractionFailed) {
        const lift::InteractionGraph& g = machine.interactionGraph();
        std::fprintf(stderr,
                     "[interaction] 任务没有走完（不是 0 点）：stop=%s detail=%s\n"
                     "  最后状态：state=%s node=%s（trace 里最后一条事件就是断点）\n",
                     lift::stopReasonName(r.reason), r.detail.c_str(), g.stateName(),
                     g.current() != nullptr ? g.current()->id.c_str() : "-");
        const std::string dump_path = interaction_fail_dump.empty()
                                          ? lift::InteractionGraph::defaultFailDumpPath(interaction_graph_path)
                                          : interaction_fail_dump;
        bool still = true;
        std::string dump_err;
        if (machine.presenter().dumpLastFrame(dump_path, &still, &dump_err)) {
            std::fprintf(stderr, "[interaction] 最后画面：%s（像素仍等于最后一次 Present 的 hash：%s）\n", dump_path.c_str(),
                         still ? "是" : "否 —— guest 可能已经画了下一帧，只作诊断");
        } else {
            std::fprintf(stderr, "[interaction] 最后画面写不出来：%s\n", dump_err.c_str());
        }
        return 5;
    }
    // ★ 交互任务的三种失败（真源 §2）：**非 0 退出**（5，可与 2 = 未实现缺口区分）+ stderr 打
    //   "最后状态与全部候选" + 存最后画面。⛔ 不许 warning-only、⛔ 不许当 0 点继续跑。
    if (r.reason == lift::StopReason::InteractionFailed) {
        // ★ 时间线 0 点没闩锁（`--anchor-script`）：**先于**其它三个分流（marker 超时 / expect-change /
        //   交互图）。复用同一个 `InteractionFailed`（退出码 5），detail 是 `origin-missed:` 那一行；
        //   ⛔ 不许落到交互图分支（图没加载时 `failureReport()` 是空串、还会往 CWD 写 `.fail.bmp`）。
        if (r.origin_missed) {
            std::fprintf(stderr, "[anchor] %s\n", r.detail.c_str());
            return 5;
        }
        // ★ marker 超时（`at marker <名字>` 等不到）：**先于** expect-change / 交互图两条分流。
        //   复用同一个 `InteractionFailed`（退出码 5），但 `detail` 是 `marker-timeout:` 那一行；
        //   ⛔ 不许落到交互图分支：图没加载时 `failureReport()` 是空串、还会往 CWD 写 `.fail.bmp`。
        if (r.marker_timeout) {
            std::fprintf(stderr, "[marker] %s\n", r.detail.c_str());
            return 5;
        }
        // ★ 时间线的效果级判据与交互图**共用**这一个停因（⛔ 不新增枚举成员），但报告出口不同：
        //   图有 `failureReport()`（节点 / 等的是什么 / 采样值），时间线只有 `RunResult.detail`
        //   （三种之一：not-changed / no-sample / unclosed）。⛔ 不许落到下面图那条分支上：
        //   图没加载时 `failureReport()` 是空串、dump 路径会变成 `".fail.bmp"`（往 CWD 写）。
        if (machine.expectChangeEnabled()) {
            std::fprintf(stderr, "[expect-change] %s\n", r.detail.c_str());
            const std::string dump_path = input_script_path + ".expect-fail.bmp";
            bool still = true;
            std::string dump_err;
            if (machine.presenter().dumpLastFrame(dump_path, &still, &dump_err)) {
                std::fprintf(stderr,
                             "[expect-change] 最后画面：%s（像素仍等于最后一次 Present 的 hash：%s）\n",
                             dump_path.c_str(),
                             still ? "是" : "否 —— guest 可能已经画了下一帧，只作诊断");
            } else {
                std::fprintf(stderr, "[expect-change] 最后画面写不出来：%s\n", dump_err.c_str());
            }
            return 5;
        }
        const lift::InteractionGraph& g = machine.interactionGraph();
        std::fputs(g.failureReport().c_str(), stderr);
        const std::string dump_path = interaction_fail_dump.empty()
                                          ? lift::InteractionGraph::defaultFailDumpPath(interaction_graph_path)
                                          : interaction_fail_dump;
        bool still = true;
        std::string dump_err;
        if (machine.presenter().dumpLastFrame(dump_path, &still, &dump_err)) {
            std::fprintf(stderr, "[interaction] 最后画面：%s（像素仍等于最后一次 Present 的 hash：%s）\n", dump_path.c_str(),
                         still ? "是" : "否 —— guest 可能已经画了下一帧，只作诊断");
        } else {
            std::fprintf(stderr, "[interaction] 最后画面写不出来：%s\n", dump_err.c_str());
        }
        return 5;
    }
    // ★ 兜底退出码抽到 `lift::exitCodeForRun`（纯函数，tests/mechanism.cpp 直接断言）：
    //   上面几道消费者闸门（token / gpu-strict / 交互图）**优先**，它们没拦下才落到这里。
    return lift::exitCodeForRun(r);
}
