// emulator-lift · **assets 档**端到端守卫：运行摘要的 `present-tail` 行
//
// 口径（这一行回答什么）：**最后一次 Present 调用 → 本次 run 停**之间的 guest 指令数。
// 红了意味着什么（判据，缺一条都不算）：
//   ① 两次子进程（A / B）都 **exit 0**；
//   ② 两份日志都有 `[input-script] finished` —— 脚本真被跑完（否则"什么都没发生就退出"也能绿）；
//   ③ 两份日志各**恰好 1 行** `^present-tail: `（0 行 = 本应 Present 却没打 / 一行都没 Present；
//      2 行 = 摘要打了两次）；
//   ④ 每一份都满足恒等式 `insns_at_last_present + tail_insns == insns`（`insns :` 那一行的值）。
//      ⚠ **这条很弱**：`tail_insns := insns − insns_at_last_present` ⇒ 只要章 ≤ `insns`，恒等式**恒真**
//      （实测：把**盖章字段本身**换成 `r.ea`、`tail_insns` 由它推导时，本用例仍 24/0 绿）⇒ 它**只**抓
//      "章 > `insns`（clamp 触发）"这一种错接。**字段接错的真正抓点是纯档 `tests/input_injection.cpp`
//      的 `present-tail-stamp`**（它要求 `insns_at_last_present` 与 shim 内记下的 `cpu.insns` 精确相等）。
//   ⑤ 每一份都满足存在性 `last_frame >= 1` ∧ `insns_at_last_present > 0` ∧ `tail_insns > 0`
//      ∧ `tail_wall_ms > 0`（章没盖 / 停在最后一次 Present 内部 / 尾段其实是 0）；
//   ⑥ 两次运行的 `present-tail` 前缀（截到 `tail_wall_ms=` **之前**）**逐字相同**
//      —— `last_frame` / `insns_at_last_present` / `tail_insns` 三个数跨运行一致 = **确定性**。
//      ⛔ 不断言 `tail_wall_ms`（宿主墙钟，只能当单样本）。
//
// ★ 判据的**盲区**（诚实登记，⛔ 不要为它造量级断言）：`tail_wall_ms` 的**单位换算**没有守卫
//   （`ms → µs` 改坏之后全 ctest 仍绿 —— 它只被当成"一个 > 0 的数"读）。
//   `--max-frames ⇒ tail_insns == 0` 同理：只有 README 的一句"实测、无用例钉"，没有用例。
//
// ⛔ 明确**不断言任何当前数字**（`last_frame == 150`、`tail_insns ≈ 131104825` 之类）：
//   它们是**读数**（进 `docs/03-emulator/observations.md`），会随构建变；断言它们只说明"仓库变了"。
//
// ★ 为什么是 `--present headless --raster gpu --audio off`（可复现档）：无 sink ⇒ 不 pump 宿主事件、
//   虚拟时钟、没有音频 feeder 线程；GPU 光栅化是**宿主**路径，不改 guest 指令流。
//   ⛔ 不加 `--max-frames`（停点在 Present 内部 ⇒ 那一行恒为 0），⛔ 不加 `--bench`。
// ★ 脚本用 `at frame 130` 而不是上一轮那个 `at frame 151`：末帧号**随构建变**，锚错会"条件永不到期
//   且静默丢弃"（`at frame` 到期了才应用，run 结束时未触发的动作被直接丢掉、一行告警都没有）。
//   本守卫不依赖末帧号（只要求 click 真的被应用）。
// ★ 两次运行**只有 `--appdata` 不同**（各自私有、全新）：共用一份会让第二次看到第一次留下的状态。
// ★ GPU / 执行源不可用 ⇒ `LIFT_SKIP`(77)：那是**环境不满足**，不是判据红。
//   `--raster gpu` 需要 SDL_GPU：先按 `tests/gpu_raster.cpp` 的判据在本进程里探一次设备。
// ★ 子进程输出走**文件重定向**（⛔ 不走管道：受限沙箱里捕获管道会 EPERM）。
//   Windows 分支与 `tests/title_entry_paths.cpp` 同形（`CreateProcessW`）；POSIX 分支只为让这条
//   守卫在 macOS 上**也能真跑**（AGENTS.md：macOS 侧做兼容性验证）。两条分支都只做一件事：
//   起子进程、把 stdout+stderr 落到一个文件、拿退出码。
// ★ **交付配置下本文件只在 Windows 的资产块里注册**（`CMakeLists.txt` 的
//   `if(EXISTS "E:/Games/Eushully/天結/install/SYS4INI.BIN" AND DEFINED ENV{LIFT_ASSETS})`）⇒
//   macOS / Linux 上不注册、不编译。POSIX 分支 + `LIFT_ASSETS_GAME_DIR` 覆盖的用途是：
//   在 macOS 上**手工编译**跑同一条判据（本仓 macOS 只做兼容性验证、不阻塞）。
//   `LIFT_ASSETS_GAME_DIR` 只覆盖 game-dir（缺省仍用 CTest 传的 `E:/Games/Eushully/天結/install`），
//   ⛔ 不覆盖档位 / 脚本 / 断言。
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "check.h"
#include "runtime/host_encoding.h"
#include "runtime/present_sdl.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#endif

namespace {

namespace fs = std::filesystem;

// 脚本：TITLE 的 Quit 在 (1190,630)（与 `.tmp/quit-tail/` 里那几份实测脚本同形）。
// ⛔ 不要在末尾加 `quit` / `wait`：本守卫要的是"引擎自己走到进程退出"的那一段尾段。
constexpr const char* kScript = "at frame 130\nclick 1190 630\n";

struct ChildResult {
    int exit_code = -1;
    bool started = false;
};

std::string readAllBytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

#if defined(_WIN32)
std::wstring quoteArg(const std::wstring& s) { return L"\"" + s + L"\""; }

// stdout 与 stderr 都重定向到**同一个**日志文件；不捕获管道。
ChildResult runChild(const fs::path& exe, const std::wstring& args, const fs::path& log_path) {
    ChildResult r;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    const HANDLE h = ::CreateFileW(log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return r;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = h;
    si.hStdError = h;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = quoteArg(exe.wstring()) + L" " + args;
    const BOOL ok = ::CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                     nullptr, &si, &pi);
    ::CloseHandle(h);
    if (!ok) return r;
    r.started = true;
    // 每次 run 给 12 分钟（两次总和 < CTest 的 TIMEOUT 1800）；卡住必须红，⛔ 不许挂死流水线。
    const DWORD waited = ::WaitForSingleObject(pi.hProcess, 720000u);
    if (waited != WAIT_OBJECT_0) {
        ::TerminateProcess(pi.hProcess, 991u);
        ::WaitForSingleObject(pi.hProcess, 10000u);
    }
    DWORD code = 0xFFFFFFFFu;
    ::GetExitCodeProcess(pi.hProcess, &code);
    r.exit_code = static_cast<int>(code);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return r;
}
#else
// POSIX：只有一个 "把 stdout+stderr 落到一个文件" 的 shell 重定向。超时交给 CTest 的 TIMEOUT。
std::string shellQuote(const std::string& s) {
    std::string out = "'";
    for (const char c : s) out += (c == '\'') ? std::string("'\\''") : std::string(1, c);
    out += "'";
    return out;
}

ChildResult runChild(const fs::path& exe, const std::vector<std::string>& args, const fs::path& log_path) {
    ChildResult r;
    std::string cmd = shellQuote(exe.string());
    for (const std::string& a : args) cmd += " " + shellQuote(a);
    cmd += " > " + shellQuote(log_path.string()) + " 2>&1";
    const int rc = std::system(cmd.c_str());
    if (rc == -1) return r;
    if (!fs::exists(log_path)) return r;  // 连日志都没写 ⇒ 根本没起来
    r.started = true;
    r.exit_code = WIFEXITED(rc) ? WEXITSTATUS(rc) : (WIFSIGNALED(rc) ? 128 + WTERMSIG(rc) : 1);
    return r;
}
#endif

// `text` 里以 `prefix` 开头的那一整行（没有 ⇒ 空串）
std::string lineStartingWith(const std::string& text, const std::string& prefix) {
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t nl = text.find('\n', pos);
        const std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (line.compare(0, prefix.size(), prefix) == 0) return line;
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return {};
}

long countLinesStartingWith(const std::string& text, const std::string& prefix) {
    long n = 0;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t nl = text.find('\n', pos);
        const std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (line.compare(0, prefix.size(), prefix) == 0) ++n;
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return n;
}

bool contains(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

// 环境不可用（**不是**判据红）：GPU / 执行源。判据照 `tests/title_entry_paths.cpp` 的
//   `environmentUnavailable()`，**补上 headless 档的那条串**（本用例是 `--present headless` ，
//   那条走 `src/main.cpp` 的 `--raster gpu（headless）失败`，不是 window 档的「要求 GPU 光栅化」）。
//   ⚠ 不要匹配 `game-dir` —— 那行每个正常启动都会打印（会把任何非 0 退出都误判成环境问题）。
bool environmentUnavailable(const std::string& log) {
    return contains(log, "要求 GPU 光栅化")                       // window 档（title_entry_paths 用这条）
           || contains(log, "--raster gpu（headless）失败")        // headless 档：拿不到 SDL_GPU 设备
           || contains(log, "窗口打开失败") || contains(log, "找不到执行源") || contains(log, "拒绝启动");
}

// 从 `line` 里取 `<key>=<十进制>`；取不到 ⇒ false（⛔ 不拿默认值冒充）。
bool readU64(const std::string& line, const char* key, std::uint64_t* out) {
    const std::string k = std::string(key) + "=";
    const std::size_t at = line.find(k);
    if (at == std::string::npos) return false;
    const char* begin = line.c_str() + at + k.size();
    char* end = nullptr;
    const unsigned long long v = std::strtoull(begin, &end, 10);
    if (end == begin) return false;
    *out = static_cast<std::uint64_t>(v);
    return true;
}

bool readDouble(const std::string& line, const char* key, double* out) {
    const std::string k = std::string(key) + "=";
    const std::size_t at = line.find(k);
    if (at == std::string::npos) return false;
    const char* begin = line.c_str() + at + k.size();
    char* end = nullptr;
    const double v = std::strtod(begin, &end);
    if (end == begin) return false;
    *out = v;
    return true;
}

// 一份日志里 `present-tail` 的解析结果。
struct Tail {
    long count = 0;              // `^present-tail: ` 的行数（判据 ③：必须恰好 1）
    std::string prefix;          // 截到 `tail_wall_ms=` **之前**（判据 ⑥：两次逐字相同）
    std::uint64_t last_frame = 0;
    std::uint64_t insns_at_last_present = 0;
    std::uint64_t tail_insns = 0;
    double tail_wall_ms = 0.0;
    bool parsed = false;         // 4 个字段都取到了
    std::uint64_t insns = 0;     // 摘要 `insns     :` 行
    bool have_insns = false;
};

Tail parseTail(const std::string& log) {
    Tail t;
    t.count = countLinesStartingWith(log, "present-tail: ");
    const std::string line = lineStartingWith(log, "present-tail: ");
    if (!line.empty()) {
        const std::size_t cut = line.find("tail_wall_ms=");
        t.prefix = cut == std::string::npos ? line : line.substr(0, cut);
        const bool ok = readU64(line, "last_frame", &t.last_frame) &&
                        readU64(line, "insns_at_last_present", &t.insns_at_last_present) &&
                        readU64(line, "tail_insns", &t.tail_insns) &&
                        readDouble(line, "tail_wall_ms", &t.tail_wall_ms);
        t.parsed = ok && cut != std::string::npos;
    }
    // `insns     : %llu`（标签补空格到 10 列再 ": "，与 `insns :` 那一行逐字一致）
    const std::string insns_line = lineStartingWith(log, "insns     : ");
    if (!insns_line.empty()) {
        const char* begin = insns_line.c_str() + std::strlen("insns     : ");
        char* end = nullptr;
        const unsigned long long v = std::strtoull(begin, &end, 10);
        if (end != begin) {
            t.insns = static_cast<std::uint64_t>(v);
            t.have_insns = true;
        }
    }
    return t;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace lift_test;
    g_current_case = "quit-tail";

    const std::vector<std::string> args = lift::argvUtf8(argc, argv);
    if (args.size() < 4) {
        std::fprintf(stderr, "用法：lift-quit-tail <emulator-lift> <game-dir> <work-dir>\n");
        return 2;
    }
    const fs::path exe = fs::path(lift::utf8ToWide(args[1]));
    // game-dir 优先级：`LIFT_ASSETS_GAME_DIR`（env，macOS 手工跑用）> argv[2]（CTest 传的
    //   `E:/Games/Eushully/天結/install`）。★ env **只**覆盖 game-dir，档位 / 脚本 / 断言不变。
    fs::path game_dir = fs::path(lift::utf8ToWide(args[2]));
    if (const char* env_game = std::getenv("LIFT_ASSETS_GAME_DIR"); env_game != nullptr && *env_game != '\0') {
        game_dir = fs::path(lift::utf8ToWide(env_game));
        std::printf("quit-tail: game-dir 被 LIFT_ASSETS_GAME_DIR 覆盖 ⇒ %s\n", game_dir.string().c_str());
    }
    const fs::path work_dir = fs::path(lift::utf8ToWide(args[3]));
    LIFT_CHECK(fs::exists(exe));
    if (!fs::exists(exe)) return finish("quit-tail");

    // `--raster gpu` 需要 SDL_GPU 设备：按 `tests/gpu_raster.cpp` 的判据先探一次。
    //   ⛔ 拿"CPU 光栅化也能跑"冒充通过：那会把档位悄悄降级成非本守卫要测的路径。
    {
        lift::SdlWindow probe;
        lift::SdlWindowOptions popt;
        popt.title = "emulator-lift quit-tail gpu probe";
        popt.driver_list = "gpu";
        popt.hidden = true;
        popt.raster_gpu = true;
        std::string perr;
        if (!probe.init(popt, &perr)) LIFT_SKIP(perr.c_str());
        if (!probe.rasterActive()) {
            probe.shutdown();
            LIFT_SKIP("rasterActive == false（拿不到 SDL_GPU 设备 ⇒ 不是判据红）");
        }
        probe.shutdown();
    }

    std::error_code ec;
    fs::create_directories(work_dir, ec);
    LIFT_CHECK(!ec);
    const fs::path script = work_dir / "script-quit.txt";
    {
        std::ofstream out(script, std::ios::binary | std::ios::trunc);
        out << kScript;
        out.close();
        LIFT_CHECK(fs::exists(script));
    }

    // 两次**逐字相同**的命令，只有 `--appdata` 是各自私有的（见文件头）。
    Tail tails[2];
    for (int i = 0; i < 2; ++i) {
        const std::string tag = i == 0 ? "a" : "b";
        const fs::path appdata = work_dir / ("appdata-" + tag);
        fs::create_directories(appdata, ec);
        const fs::path log_path = work_dir / ("quit-tail-" + tag + ".log");
        fs::remove(log_path, ec);

        std::vector<std::string> child;
        child.push_back("--game-dir"), child.push_back(game_dir.string());
        child.push_back("--appdata"), child.push_back(appdata.string());
        child.push_back("--skip-logo");
        child.push_back("--audio"), child.push_back("off");
        child.push_back("--present"), child.push_back("headless");
        child.push_back("--raster"), child.push_back("gpu");
        child.push_back("--time-limit"), child.push_back("600");
        child.push_back("--input-script"), child.push_back(script.string());

#if defined(_WIN32)
        std::wstring wargs;
        for (const std::string& a : child) wargs += L" " + quoteArg(lift::utf8ToWide(a));
        const ChildResult r = runChild(exe, wargs, log_path);
#else
        const ChildResult r = runChild(exe, child, log_path);
#endif
        if (!r.started) LIFT_SKIP("起不了 emulator-lift 子进程（exe 路径 / 沙箱限制？）");
        const std::string log = readAllBytes(log_path);
        if (log.empty()) LIFT_SKIP("子进程没写日志（工作目录 / 权限问题）");
        std::printf("quit-tail: run=%s exit=%d log=%s (%zu 字节)\n", tag.c_str(), r.exit_code,
                    log_path.string().c_str(), log.size());
        // ★ 环境豁免必须排在**第一个 `LIFT_CHECK` 之前**：`LIFT_SKIP` 在 `g_failures > 0` 时
        //   会 `return 1`（`tests/check.h`），排在断言之后就变成"环境问题报 FAIL"。
        if (r.exit_code != 0 && environmentUnavailable(log)) {
            LIFT_SKIP("GPU / 执行源不可用 ⇒ 这条不是判据红");                        // 同 title_entry_paths
        }
        tails[i] = parseTail(log);
        if (!tails[i].parsed) std::printf("quit-tail: run=%s 原始 present-tail 行：%s\n", tag.c_str(),
                                          lineStartingWith(log, "present-tail: ").c_str());
        else
            std::printf("quit-tail: run=%s %s\n", tag.c_str(), lineStartingWith(log, "present-tail: ").c_str());

        // ⚠ `g_current_case` 是 `const char*` ⇒ 名字要存在 static 里（⛔ 不能指向临时 string）
        static std::string case_name;
        case_name = std::string("quit-tail/") + tag;
        g_current_case = case_name.c_str();
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(r.exit_code), 0u);              // ①
        LIFT_CHECK(contains(log, "[input-script] finished"));                        // ②
        LIFT_CHECK_EQ_I64(tails[i].count, 1);                                        // ③ 恰好 1 行
        LIFT_CHECK(tails[i].parsed);                                                 // 4 个字段都解析出来
        LIFT_CHECK(tails[i].have_insns);                                             // `insns     :` 也在
        if (tails[i].parsed && tails[i].have_insns) {
            // ④ 恒等式（⚠ 只抓"章 > insns / clamp 触发"这一种错接；字段接错靠纯档 present-tail-stamp）
            LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(tails[i].insns_at_last_present + tails[i].tail_insns),
                              static_cast<std::int64_t>(tails[i].insns));
        }
        LIFT_CHECK(tails[i].last_frame >= 1);                                        // ⑤ 存在性
        LIFT_CHECK(tails[i].insns_at_last_present > 0);                              //   章真的盖了
        LIFT_CHECK(tails[i].tail_insns > 0);                                         //   确实有尾段
        LIFT_CHECK(tails[i].tail_wall_ms > 0.0);                                     //   墙钟不是 0
        if (g_failures > 0) break;  // 第一份就红 ⇒ 第二份的读数也不该被当真
    }

    // ⑥ 跨运行：`last_frame` / `insns_at_last_present` / `tail_insns` 三个数必须逐字相同
    //   （只截到 `tail_wall_ms=` **之前** ⇒ 宿主墙钟不参与比较）。
    if (tails[0].parsed && tails[1].parsed) {
        g_current_case = "quit-tail/deterministic";
        if (tails[0].prefix != tails[1].prefix) {
            std::fprintf(stderr, "  run=a: %s\n  run=b: %s\n", tails[0].prefix.c_str(), tails[1].prefix.c_str());
        }
        LIFT_CHECK(tails[0].prefix == tails[1].prefix);
    }

    return finish("quit-tail");
}
