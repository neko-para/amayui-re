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
#include <string>
#include <vector>

#include "corpus/lst_index.h"
#include "runtime/machine.h"
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
        "  --agerc <path>      AGERC.DLL 的**拆壳件**（真实 guest 模块装载；缺省 = --game-dir 下的 AGERC.DLL）\n"
        "  --real-time         用宿主时钟（此时 A4 的逐字节断言自动跳过并明说）\n"
        "  --trace-inputs      打印 trace 输入清单（exe 路径串 / guest 命令行 / 环境块 / 时钟起点）\n"
        "  --until shim        跑到第一次 shim 调用就停（L1）\n"
        "  --until ea <hex>    跑到指定 EA 就停（L2 = 0x4BA890）\n"
        "  --max-insns <N>     最多执行 N 条指令\n"
        "  --time-limit <秒>   墙钟上限（探索用；0 = 不限，缺省）\n"
        "  --trace-shims       打印 shim 调用（确定性格式）\n"
        "  --trace-blocks      打印每条执行的指令（调试用，很慢）\n"
        "  --safe-memory       每次访存都查提交位图（A1 的第二遍）\n"
        "  --report            打印基座缺口（A5）后退出\n"
        "  --bench <N>         跑 N 条指令并报 MIPS\n"
        "  --smoke             只验证宿主层可链接（不建窗口、不需要游戏）\n");
}

}  // namespace

int main(int argc, char** argv) {
    bool smoke = false;
    bool trace_shims = false;
    bool trace_blocks = false;
    bool safe_memory = false;
    bool report = false;
    bool real_time = false;
    bool trace_inputs = false;
    std::uint64_t bench = 0;
    std::uint32_t max_insns = 0;
    double time_limit_sec = 0.0;
    std::uint32_t until_ea = 0;
    bool until_shim = false;
    std::string exe_path;
    std::string lst_path;
    std::string game_dir;
    std::string appdata_dir;
    std::string guest_root;
    std::string agerc_path;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--smoke") == 0) {
            smoke = true;
        } else if (std::strcmp(a, "--exe") == 0 && i + 1 < argc) {
            exe_path = argv[++i];
        } else if (std::strcmp(a, "--lst") == 0 && i + 1 < argc) {
            lst_path = argv[++i];
        } else if (std::strcmp(a, "--game-dir") == 0 && i + 1 < argc) {
            game_dir = argv[++i];
        } else if (std::strcmp(a, "--appdata") == 0 && i + 1 < argc) {
            appdata_dir = argv[++i];
        } else if (std::strcmp(a, "--agerc") == 0 && i + 1 < argc) {
            agerc_path = argv[++i];
            if (agerc_path.empty()) {
                std::fprintf(stderr, "--agerc 需要一个非空路径\n");
                return 2;
            }
        } else if (std::strcmp(a, "--guest-root") == 0 && i + 1 < argc) {
            guest_root = argv[++i];
        } else if (std::strcmp(a, "--real-time") == 0) {
            real_time = true;
        } else if (std::strcmp(a, "--trace-inputs") == 0) {
            trace_inputs = true;
        } else if (std::strcmp(a, "--until") == 0 && i + 1 < argc) {
            const char* what = argv[++i];
            if (std::strcmp(what, "shim") == 0) {
                until_shim = true;
            } else if (std::strcmp(what, "ea") == 0 && i + 1 < argc) {
                until_ea = parseHex(argv[++i]);
            } else {
                std::fprintf(stderr, "--until 需要 shim 或 ea <hex>\n");
                return 2;
            }
        } else if (std::strcmp(a, "--max-insns") == 0 && i + 1 < argc) {
            max_insns = static_cast<std::uint32_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (std::strcmp(a, "--time-limit") == 0 && i + 1 < argc) {
            time_limit_sec = std::strtod(argv[++i], nullptr);
            if (time_limit_sec <= 0.0) {
                std::fprintf(stderr, "--time-limit 需要正数秒\n");
                return 2;
            }
        } else if (std::strcmp(a, "--bench") == 0 && i + 1 < argc) {
            bench = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(a, "--trace-shims") == 0) {
            trace_shims = true;
        } else if (std::strcmp(a, "--trace-blocks") == 0) {
            trace_blocks = true;
        } else if (std::strcmp(a, "--safe-memory") == 0) {
            safe_memory = true;
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

    lift::RunOptions opt;
    opt.until_ea = until_ea;
    opt.stop_on_first_shim = until_shim;
    opt.max_insns = bench ? bench : max_insns;
    opt.time_limit_sec = time_limit_sec;
    opt.trace_shims = trace_shims;
    opt.trace_blocks = trace_blocks;
    opt.real_time = real_time;
    opt.trace_inputs = trace_inputs || trace_shims;
    if (real_time) {
        std::fprintf(stderr,
                     "emulator-lift: ⚠ --real-time：宿主时钟进 guest ⇒ **A4 的逐字节可复现断言自动跳过**"
                     "（trace 输入清单里的 clock=REAL-TIME）\n");
    }

    const std::uint64_t t0 = SDL_GetTicksNS();
    const lift::RunResult r = machine.run(opt);
    const std::uint64_t t1 = SDL_GetTicksNS();

    machine.dumpRunSummary(r, stdout);
    if (bench) {
        const double secs = static_cast<double>(t1 - t0) / 1e9;
        std::printf("bench     : %llu insns in %.3f s = %.2f MIPS\n", (unsigned long long)r.insns, secs,
                    secs > 0 ? static_cast<double>(r.insns) / secs / 1e6 : 0.0);
    }
    machine.reportBaseGaps(stdout);
    return r.ok() ? 0 : 2;
}
