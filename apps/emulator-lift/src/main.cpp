// emulator-lift · 入口 / CLI
//
// ★ 默认路径（README §2.1 的定位顺序）：`--exe` → 环境变量 → 仓库内入库件 → 拒绝启动。
//   ⛔ 不回退到游戏安装目录里的原版 exe（那是 binary/age-original 的兄弟，节表未还原）。
// ★ 启动前校验：PE 的 sha256 必须等于 `.lst` 头部的 `; Input SHA256 :`（plan §4 D1 / A3 ⓪）。
//   不符 ⇒ 打印两侧并拒绝启动；`.lst` 缺失 ⇒ 打印"无法校验"（⛔ 不假装校验过）。
// ★ 用法：
//     emulator-lift --smoke
//     emulator-lift [--exe <path>] [--lst <path>] [--until shim | --until ea <hex>]
//                   [--max-insns N] [--trace-shims] [--trace-blocks] [--safe-memory] [--bench N]
#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "corpus/lst_index.h"
#include "runtime/font.h"
#include "runtime/host_encoding.h"
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
        "  --trace-inputs      打印 trace 输入清单（exe 路径串 / guest 命令行 / 环境块 / 时钟起点）\n"
        "  --until shim        跑到第一次 shim 调用就停（L1）\n"
        "  --until ea <hex>    跑到指定 EA 就停（L2 = 0x4BA890）\n"
        "  --max-insns <N>     最多执行 N 条指令\n"
        "  --max-frames <N>    最多呈现 N 帧后停（0 = 不限；D3D9 Present 计数）\n"
        "  --dump-frame <path> 每次 Present 导出 BMP；路径里的 {frame} 换成 4 位帧号\n"
        "                      （不含 {frame} 时同一文件会被覆盖；配合 --max-frames 1 取第一帧）\n"
        "  --time-limit <秒>   墙钟上限（探索用；0 = 不限，缺省）\n"
        "  --trace-shims       打印 shim 调用（确定性格式）\n"
        "  --trace-blocks      打印每条执行的指令（调试用，很慢）\n"
        "  --safe-memory       每次访存都查提交位图（A1 的第二遍）\n"
        "  --report            打印基座缺口（A5）后退出\n"
        "  --bench <N>         跑 N 条指令并报 MIPS\n"
        "  --profile           跑完后打印 guest 热点（块级 + 按 .lst 函数入口聚合）\n"
        "  --profile-top <N>   profile 每张表打印前 N 项（缺省 30）\n"
        "  --present <mode>    headless（缺省）/ window / auto：呈现后端；window = SDL 窗口 + GPU API 路径\n"
        "  --present-driver <list>  逗号列表（缺省 gpu,direct3d12,direct3d11,vulkan,metal,opengl）\n"
        "  --present-vsync 0|1       窗口垂直同步（缺省 0；1 会改变墙钟）\n"
        "  --present-hidden          隐藏窗口（CTest / 无人值守）\n"
        "  --present-screenshot <path>  窗口像素 BMP（present 前回读；支持 {frame}）\n"
        "  --font <path>       宿主字体（缺省：CMake 注入的 Amayui CN；调试覆盖）\n"
        "  --native-overrides <list>  逗号分隔的 guest 函数名（或 all）：函数级 native override（缺省关）\n"
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
    bool trace_inputs = false;
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
    int present_vsync = 0;
    bool present_hidden = false;
    bool present_driver_set = false;
    bool present_vsync_set = false;
    bool present_screenshot_set = false;
    int baseline_lift = 2;
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
            time_limit_sec = std::strtod(args[++i].c_str(), nullptr);
            if (time_limit_sec <= 0.0) {
                std::fprintf(stderr, "--time-limit 需要正数秒\n");
                return 2;
            }
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
        } else if (std::strcmp(a, "--trace-shims") == 0) {
            trace_shims = true;
        } else if (std::strcmp(a, "--trace-blocks") == 0) {
            trace_blocks = true;
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

    // ---- 呈现后端参数校验（缺省 headless；窗口参数给了就必须显式 --present window/auto）----
    if (present_mode != "headless" && present_mode != "window" && present_mode != "auto") {
        std::fprintf(stderr, "--present 只能是 headless / window / auto（收到 %s）\n", present_mode.c_str());
        return 2;
    }
    const bool present_window_flag =
        present_driver_set || present_vsync_set || present_hidden || present_screenshot_set;
    if (present_mode == "headless" && present_window_flag) {
        std::fprintf(stderr, "--present headless 不接受窗口参数（--present-driver / --present-vsync / "
                             "--present-hidden / --present-screenshot）\n");
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
    if (game_dir.empty()) game_dir = envOr("AMAYUI_GAME_DIR");
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
        std::printf("native-ovr: %zu 项（--native-overrides=%s）\n", machine.overrides().size(),
                    native_overrides.c_str());
        for (std::size_t i = 0; i < machine.overrides().size(); ++i) {
            const lift::NativeOverride* e = machine.overrides().at(i);
            std::printf("  %-12s @ %08X %s(%uB)\n", e->name.c_str(), e->ea, lift::guestConvName(e->conv),
                        e->arg_bytes);
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
        auto sdl = std::make_unique<lift::SdlWindow>();
        std::string werr;
        if (!sdl->init(wopt, &werr)) {
            if (present_mode == "auto") {
                std::fprintf(stderr, "emulator-lift: --present auto：窗口打开失败，回退 headless：%s\n",
                             werr.c_str());
                if (!present_screenshot.empty()) {
                    std::fprintf(stderr, "  ⚠ --present-screenshot 不会产出（headless 没有窗口像素）\n");
                }
            } else {
                std::fprintf(stderr, "emulator-lift: --present window 失败：%s\n", werr.c_str());
                return 2;
            }
        } else {
            std::printf("present   : %s\n", sdl->describe().c_str());
            if (!sdl->isGpuRenderer()) {
                std::printf("present   : 注意：driver=%s 是 GPU API 路径，但不是 SDL_GPU（gpu driver）\n",
                            sdl->rendererName());
            }
            machine.presenter().setSink(std::move(sdl));
            present_window_active = true;
        }
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

    const std::uint64_t t0 = SDL_GetTicksNS();
    const lift::RunResult r = machine.run(opt);
    const std::uint64_t t1 = SDL_GetTicksNS();

    machine.dumpRunSummary(r, stdout);
    if (profile) machine.dumpProfile(stdout, profile_top);
    if (bench) {
        const double secs = static_cast<double>(t1 - t0) / 1e9;
        std::printf("bench     : %llu insns in %.3f s = %.2f MIPS\n", (unsigned long long)r.insns, secs,
                    secs > 0 ? static_cast<double>(r.insns) / secs / 1e6 : 0.0);
    }
    machine.reportBaseGaps(stdout);
    return r.ok() ? 0 : 2;
}
