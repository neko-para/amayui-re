// emulator-lift · **assets 档**端到端守卫：`graphs/sn0000-quit.graph` 的 TITLE→Quit 必须按引擎语义收口
//
// REQ-01M77MC1374HXC2JR7E0ZYSHQJ（用户实测 `stop : unimplemented @ eip=0x109A5A71`）。
//
// 红了意味着什么：Quit 的 `_CxxThrowException`（0x418E74）派发到了一条**不属于当前线程**的 SEH 链
//   —— `fs:[0]` 的链头落在**音频 feeder 的栈**上（或 feeder 把一处已过期的 record 写回主线程），
//   那条 record 的 `Handler` 已被复用成数据 ⇒ 间接调用把 guest 堆 / `.data` 当地址执行，最终以
//   `stop : unimplemented`（或 `decode-error`）停。玩家侧看到的是"点了退出，游戏没退"。
//
// 判据（缺一条都不算）：
//   ① 子进程真的跑起来了（起不了 ⇒ SKIP 77：exe 路径 / 沙箱限制）；
//   ② 这一臂必须是**真 feeder 臂**：日志里有 `feeder=host-thread` 与 `device=ok`
//      （`thread_mode_ = Sdl && realTime`）。缺一条 ⇒ SKIP 77 —— **不是判据红**：没有 feeder 线程
//      就没有这条并发通道，量错了臂。⛔ 不许把"没跑"记成"跑绿了"。
//   ③ stop = `stop      : stopped`，detail 含 `kernel32!ExitProcess`（引擎自己的退出路径）；
//   ④ **没有** `unimplemented` / `decode-error` 的停因（`stop` 行与 `=== unimplemented` 块都算）；
//   ⑤ 同一 graph 跑**两遍**（两个独立 `--appdata`）都满足 ③④。
//      ★ 为什么是两遍：这条缺陷依赖 feeder 的并发窗口 —— 修前实测 3/4 次红，且每次 EIP 都不同
//        （`0x109C0341` / `0x103676C1` / `0x10357221`，以及 `decode-error @ 0x00544774`），
//        单跑一次可能假绿。修后是**结构性**的（feeder 有自己的 TEB，主线程链头不再被碰）⇒ 确定性绿。
//
// ★ 为什么必须 `--present window --present-hidden`：
//   · headless 下 `wait … wall_ms` 与帧预算跑歪（实测停在 step3「等 GAMESTART」超时）；
//   · `--audio off` / 非 real-time ⇒ 没有 feeder 真线程 ⇒ 根本量不到这条通道；
//   · 所以这个判据**只能在真窗口臂上成立**；窗口 / GPU / 音频设备不可用 ⇒ SKIP 77。
// ★ 判据取"正面结局"（stopped + ExitProcess），⛔ 不写成"只要不是 unimplemented 就行"：
//   那样任何别的停因（no-code / 资源耗尽 / 超时）都会假绿。
// ★ 图的最后一个节点是 `wait 10000 wall_ms`，ExitProcess 会让它**永远等不到** ⇒ 主进程退出码是 5
//   （交互任务"没走完"）；那是这张图的形状，不是判据。⛔ 不要断言 exit 0。
// ★ 子进程输出走**文件重定向**（⛔ 不走管道：受限沙箱里捕获管道会 EPERM）。
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
#include "runtime/host_encoding.h"  // `lift::utf8ToWide`（Windows 的命令行构造）

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
using namespace lift_test;

struct ChildResult {
    int exit_code = -1;
    bool started = false;
};

#if defined(_WIN32)
std::wstring quoteArg(const std::wstring& s) { return L"\"" + s + L"\""; }

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
    // CTest 的 TIMEOUT 是 1800 s；这里给 2×600 s（两遍）× 余量 = 1500 s（卡住必须红，⛔ 不许挂死）。
    const DWORD waited = ::WaitForSingleObject(pi.hProcess, 1500000u);
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
    if (!fs::exists(log_path)) return r;
    r.started = true;
    r.exit_code = WIFEXITED(rc) ? WEXITSTATUS(rc) : (WIFSIGNALED(rc) ? 128 + WTERMSIG(rc) : 1);
    return r;
}
#endif

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool contains(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

// 失败时把**判据读数的现场**打出来：stop / detail / `[esp]` / region / 最近 24 条 ring。
// ★ 只打印日志里已经有的行（⛔ 不在这里编造任何读数）。
void printFailureExcerpt(const std::string& log) {
    std::fprintf(stderr, "---- 判据现场（子进程日志摘录）----\n");
    std::size_t pos = 0;
    while (pos < log.size()) {
        const std::size_t end = log.find('\n', pos);
        const std::string line = log.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        if (contains(line, "stop      :") || contains(line, "detail    :") ||
            contains(line, "[interaction] 任务") || contains(line, "数据地址当 eip") ||
            contains(line, "guest stack (esp=") || contains(line, "region :")) {
            std::fprintf(stderr, "  %s\n", line.c_str());
        }
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    const std::size_t block = log.find("=== unimplemented instruction");
    if (block != std::string::npos) {
        std::fprintf(stderr, "  --- `=== unimplemented instruction` 块（前 40 行）---\n");
        std::size_t p = block;
        for (int i = 0; i < 40 && p < log.size(); ++i) {
            const std::size_t end = log.find('\n', p);
            std::fprintf(stderr, "  %s\n",
                         log.substr(p, end == std::string::npos ? std::string::npos : end - p).c_str());
            if (end == std::string::npos) break;
            p = end + 1;
        }
    }
    std::fprintf(stderr, "---- 摘录结束 ----\n");
}

// 一条臂 = 一次子进程。返回 true = 这一臂满足全部判据（③④）。
bool runArm(const fs::path& exe, const fs::path& game_dir, const fs::path& graph, const fs::path& work,
            const char* tag, bool* environment_unavailable) {
    std::error_code ec;
    const fs::path appdata = work / (std::string("appdata-") + tag);
    fs::remove_all(appdata, ec);
    fs::create_directories(appdata, ec);
    const fs::path log_path = work / (std::string("quit-") + tag + ".log");
    fs::remove(log_path, ec);
    const fs::path fail_bmp = work / (std::string("quit-") + tag + ".fail.bmp");

    std::vector<std::string> child;
    child.push_back("--appdata");
    child.push_back(appdata.string());
    child.push_back("--game-dir");
    child.push_back(game_dir.string());
    child.push_back("--present");
    child.push_back("window");
    child.push_back("--present-hidden");
    child.push_back("--raster");
    child.push_back("gpu");
    child.push_back("--native-overrides");
    child.push_back("all");
    child.push_back("--skip-logo");
    child.push_back("--interaction-graph");
    child.push_back(graph.string());
    // 失败时的"最后画面"写进 work（缺省会写 `<graph>.fail.bmp`，那是源码树）。
    child.push_back("--interaction-fail-dump");
    child.push_back(fail_bmp.string());
    child.push_back("--max-frames");
    child.push_back("4000");
    child.push_back("--time-limit");
    child.push_back("300");
    // `feeder=host-thread` 那行是 Info 级（缺省 warn 会把它滤掉）⇒ ② 的判据需要它。
    // ★ 同时把分类收到 `dsound,general`：完整 `--verbose` 会打出上百万行 `[shim]`（单臂 ~60 MB），
    //   而本测试只需要音频臂的判据行（`[audio]` 走 `LogDsound`）。
    child.push_back("--verbose");
    child.push_back("--log-cat");
    child.push_back("dsound,general");

#if defined(_WIN32)
    std::vector<std::wstring> wargs;
    wargs.reserve(child.size());
    for (const std::string& a : child) wargs.push_back(lift::utf8ToWide(a));
    std::wstring joined;
    for (const std::wstring& a : wargs) joined += L" " + quoteArg(a);
    const ChildResult r = runChild(exe, joined, log_path);
#else
    const ChildResult r = runChild(exe, child, log_path);
#endif
    if (!r.started) {
        std::printf("quit-graph[%s]: 子进程起不来（exe 路径 / 沙箱限制）\n", tag);
        *environment_unavailable = true;
        return false;
    }
    const std::string log = readAll(log_path);
    std::printf("quit-graph[%s]: exit=%d log=%s (%zu 字节)\n", tag, r.exit_code, log_path.string().c_str(),
                log.size());
    if (log.empty()) {
        std::printf("quit-graph[%s]: 子进程没写日志（工作目录 / 权限）\n", tag);
        *environment_unavailable = true;
        return false;
    }
    // ---- ② 真 feeder 臂？（缺 ⇒ SKIP，不是判据红）----
    if (!contains(log, "feeder=host-thread") || !contains(log, "device=ok")) {
        std::printf("quit-graph[%s]: 不是真 feeder 臂（缺 `feeder=host-thread` 或 `device=ok`）\n", tag);
        *environment_unavailable = true;
        return false;
    }
    // ★ 环境判据就只有上面那一条（真 feeder 臂）。⛔ 不要用 `log` 里出现"执行源"/"GPU"之类的
    //   泛词做跳过判据 —— 运行头就会打 `执行源 : …`，那会把一次**正常的绿**误判成 SKIP
    //   （实测踩过：第一版用 `contains(log, "执行源")` ⇒ 整个用例 skipped 64 s）。

    static std::string case_name;
    case_name = std::string("quit-graph/") + tag;
    g_current_case = case_name.c_str();

    // ---- ③ 正面结局：引擎自己走到 ExitProcess ----
    const bool stopped = contains(log, "stop      : stopped");
    const bool exit_process = contains(log, "kernel32!ExitProcess");
    // ---- ④ 反面：没有"数据地址当 eip"的停因 ----
    const bool bad_stop = contains(log, "stop      : unimplemented") || contains(log, "stop      : decode-error") ||
                          contains(log, "=== unimplemented instruction") ||
                          contains(log, "数据地址当 eip");
    LIFT_CHECK(stopped);
    LIFT_CHECK(exit_process);
    LIFT_CHECK(!bad_stop);
    if (!(stopped && exit_process && !bad_stop)) printFailureExcerpt(log);
    return stopped && exit_process && !bad_stop;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr, "用法：%s <emulator-lift> <game-dir> <graph> <work-dir>\n", argv[0]);
        return 2;
    }
    const fs::path exe_arg = argv[1];
    const fs::path game_dir = argv[2];
    const fs::path graph = argv[3];
    const fs::path work_dir = argv[4];
    if (!fs::exists(exe_arg)) {
        std::printf("SKIP: 没有 emulator-lift 可执行文件（%s）\n", exe_arg.string().c_str());
        return 77;
    }
    if (!fs::exists(game_dir / "SYS4INI.BIN")) {
        std::printf("SKIP: 安装树里没有 SYS4INI.BIN（%s）\n", game_dir.string().c_str());
        return 77;
    }
    if (!fs::exists(graph)) {
        std::printf("SKIP: 没有 interaction-graph（%s）\n", graph.string().c_str());
        return 77;
    }
    std::error_code ec;
    fs::create_directories(work_dir, ec);

    bool environment_unavailable = false;
    const bool arm1 = runArm(exe_arg, game_dir, graph, work_dir, "a", &environment_unavailable);
    if (environment_unavailable) LIFT_SKIP("窗口 / GPU / 音频设备 / 执行源不可用 ⇒ 这条不是判据红");
    const bool arm2 = runArm(exe_arg, game_dir, graph, work_dir, "b", &environment_unavailable);
    if (environment_unavailable) LIFT_SKIP("第二臂环境不可用（见上）⇒ 这条不是判据红");
    LIFT_CHECK(arm1);
    LIFT_CHECK(arm2);
    return finish("quit-graph");
}
