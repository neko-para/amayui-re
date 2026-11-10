// emulator-lift · **assets 档**端到端守卫：TITLE 之后两条入口路径不许崩
//
// 红了意味着什么（判据，缺一条都不算）：
//   ① 两个子进程（A / B）都 **exit 0**；
//   ② 都跑到脚本**最后一条 `quit`**（run summary 的 `detail    : input script: quit`）
//      —— 这是"进入目标画面之后仍然活着"的机械判据（⛔ 不是"我看了一眼截图"）；
//   ③ 日志里**没有**崩溃 / 停机痕迹：
//      `软件中断`、`guest interrupt`、`int 3（调试陷阱`、`stop      : unimplemented`、
//      `=== guest memory fault ===`、`guest 堆耗尽`、`unimplemented import`；
//   ④ 脚本里的 `click` 都**真的被应用**了（A = 2 条、B = 1 条）—— 否则"quit 到得快"
//      会让 ①②③ 全部空转通过（这是本条最容易被写成恒真的地方）。
//
// ★ 钉的是什么缺陷（2026-11-10）：这两条路径都在 `0x4586AA` 抛
//   `std::out_of_range("invalid vector<T> subscript")`（`sub_4581F0` 查**字体名表**失败后
//   拿 -1 当下标），崩溃栈：`0x4EDB8B → 0x4BB51C → 0x41346D → 0x42346B → 0x4567DA →
//   0x4716B0 → 0x458A30 → 0x4581F0 → 0x428990 → 0x4586AA`。
//   根因是 `EnumFontFamiliesExA` 的**枚举条数**（引擎 boot 时用它建字体名表）：只报宿主
//   族名一条时，引擎查 `ＭＳ ゴシック` 必然 -1 ⇒ 抛。判据与修法见 `src/shims/gdi32_font.cpp`
//   的 `kEnumFamilyNames` / 单元判据 `tests/gdi.cpp` 的 `enum-font-families*`。
//
// ★ 脚本里的 `wait` 是**判据的一部分**，不是装饰：`quit` 若与最后一次 `click` 同一帧生效，
//   目标画面还没建起来就退出了 ⇒ 崩溃根本不会发生 ⇒ 这条守卫会恒绿（实测：去掉 wait 之后
//   修前也是 exit 0）。加了 wait 之后修前**必然红**（A：`=== guest memory fault ===` +
//   exit 3；B：`stop: unimplemented @ eip=004E6E3F` + `软件中断` + exit 2）。
//
// ★ 为什么用 `--present window --present-hidden --raster gpu`：`--present headless` 是 CPU
//   光栅化（实测 490 帧要 10 分钟以上）；窗口隐藏仍无焦点、无真实光标。
// ★ GPU / 窗口 / 安装不可用 ⇒ `LIFT_SKIP`(77)（那是"环境不满足"，不是"判据红"）。
// ★ 子进程输出走**文件重定向**（不走管道：受限沙箱里捕获管道会 EPERM）。
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "check.h"
#include "runtime/host_encoding.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

namespace fs = std::filesystem;

// A：TITLE → OPTION → 左侧「ADV设定」（进 ADV 设定画面时崩在 0x4586AA）
// B：TITLE → GameStart（进游戏时崩在同一处）
// ★ 帧号 / 坐标与工单一致；末尾的 `wait` 见文件头（缺它这条守卫会恒绿）。
struct PathCase {
    const char* id;
    const char* script;  // UTF-8 正文
    int clicks;          // 脚本里 click 的条数（判据 ④）
};

constexpr PathCase kCases[] = {
    {"a", "at frame 130\nclick 812 622\nat frame 200\nclick 112 216\nwait 100 frames\nquit\n", 2},
    {"b", "at frame 130\nclick 1175 355\nwait 100 frames\nquit\n", 1},
};

std::string readAllBytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::wstring quoteArg(const std::wstring& s) { return L"\"" + s + L"\""; }

long countAppliedClicks(const std::string& log) {
    long n = 0;
    std::size_t at = 0;
    const std::string key = "[input-script] frame=";
    while ((at = log.find(key, at)) != std::string::npos) {
        const std::size_t eol = log.find('\n', at);
        const std::string line = log.substr(at, eol == std::string::npos ? std::string::npos : eol - at);
        if (line.find(" click ") != std::string::npos) ++n;
        at += key.size();
    }
    return n;
}

bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

#if defined(_WIN32)
struct ChildResult {
    DWORD exit_code = 0xFFFFFFFFu;
    bool started = false;
};

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
    // 4 分钟上限（正常一条 15~60 s；卡住时必须红，而不是挂死流水线）。
    const DWORD waited = ::WaitForSingleObject(pi.hProcess, 240000u);
    if (waited != WAIT_OBJECT_0) {
        ::TerminateProcess(pi.hProcess, 991u);
        ::WaitForSingleObject(pi.hProcess, 10000u);
    }
    ::GetExitCodeProcess(pi.hProcess, &r.exit_code);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return r;
}
#endif

// 环境不可用（不是判据红）：GPU / 窗口 / 执行源。⚠ 不要匹配 `game-dir` —— 那行每个
// 正常启动都会打印（会把任何非 0 退出都误判成环境问题）。
bool environmentUnavailable(const std::string& log) {
    return contains(log, "要求 GPU 光栅化") || contains(log, "窗口打开失败") ||
           contains(log, "找不到执行源") || contains(log, "拒绝启动");
}

// 崩溃 / 停机痕迹（判据 ③）。分组注释写清每条来自哪次实测。
bool crashMarker(const std::string& log) {
    return contains(log, "软件中断")                       // 修前 B：int 3 / int imm8 停机
           || contains(log, "guest interrupt")             // 同上（英文摘要）
           || contains(log, "int 3（调试陷阱")             // Int3 单独一条 detail
           || contains(log, "stop      : unimplemented")   // 任何 unimplemented 停机
           || contains(log, "=== guest memory fault ===")  // 修前 A：SIGSEGV（同一条抛点）
           || contains(log, "guest 堆耗尽")                 // 堆打满 ⇒ 后续行为不可信
           || contains(log, "unimplemented import");       // 命中未实现的导入
}

}  // namespace

int main(int argc, char** argv) {
    using namespace lift_test;
    g_current_case = "title-entry-paths";
#if !defined(_WIN32)
    (void)argc;
    (void)argv;
    LIFT_SKIP("本守卫用 CreateProcessW 起子进程（Windows 专有）；非 Windows 侧不跑");
#else
    const std::vector<std::string> args = lift::argvUtf8(argc, argv);
    if (args.size() < 4) {
        std::fprintf(stderr, "用法：lift-title-entry-paths <emulator-lift.exe> <game-dir> <work-dir>\n");
        return 2;
    }
    const fs::path exe = fs::path(lift::utf8ToWide(args[1]));
    const fs::path game_dir = fs::path(lift::utf8ToWide(args[2]));
    const fs::path work_dir = fs::path(lift::utf8ToWide(args[3]));
    std::error_code ec;
    fs::create_directories(work_dir, ec);

    for (const PathCase& pc : kCases) {
        const std::string id = pc.id;
        const fs::path script = work_dir / ("script-" + id + ".txt");
        {
            std::ofstream out(script, std::ios::binary | std::ios::trunc);
            out << pc.script;
        }
        // ★ `--appdata` 必须是**私有**目录（一次运行一个）：引擎会在里面写配置 / 临时文件，
        //   两条路径共享一份会让第二条看到第一条留下的状态（⛔ 不是本守卫要测的东西）。
        const fs::path appdata = work_dir / ("appdata-" + id);
        fs::create_directories(appdata, ec);
        const fs::path log_path = work_dir / ("path-" + id + ".log");
        std::wstring child_args;
        child_args += L"--appdata " + quoteArg(appdata.wstring());
        child_args += L" --game-dir " + quoteArg(game_dir.wstring());
        child_args += L" --skip-logo --present window --present-hidden --raster gpu";
        child_args += L" --input-script " + quoteArg(script.wstring());
        child_args += L" --max-frames 400 --time-limit 300";
        const ChildResult r = runChild(exe, child_args, log_path);
        if (!r.started) LIFT_SKIP("起不了 emulator-lift 子进程（exe 路径不对？）");
        const std::string log = readAllBytes(log_path);
        if (log.empty()) LIFT_SKIP("子进程没写日志（工作目录 / 权限问题）");
        std::printf("title-entry-paths: path=%s exit=%lu log=%s (%zu 字节)\n", id.c_str(),
                    static_cast<unsigned long>(r.exit_code), log_path.string().c_str(), log.size());
        if (r.exit_code != 0 && environmentUnavailable(log)) {
            LIFT_SKIP("GPU / 窗口 / 游戏安装不可用 ⇒ 这条不是判据红");
        }
        // ⚠ `g_current_case` 是 `const char*` ⇒ 名字要存在 static 里（⛔ 不能指向临时 string）
        static std::string case_name;
        case_name = std::string("title-entry-paths/") + id;
        g_current_case = case_name.c_str();
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(r.exit_code), 0u);            // ①
        LIFT_CHECK(contains(log, "detail    : input script: quit"));               // ②
        LIFT_CHECK(!crashMarker(log));                                            // ③
        const long clicks = countAppliedClicks(log);
        std::printf("title-entry-paths: path=%s clicks=%ld（脚本 %d 条）\n", id.c_str(), clicks, pc.clicks);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(clicks),                        // ④
                          static_cast<std::uint32_t>(pc.clicks));
        if (g_failures > 0) break;  // 第一条就红 ⇒ 第二条的日志也不该被当真
    }
    return finish("title-entry-paths");
#endif
}
