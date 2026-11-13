// emulator-lift · **assets 档**端到端守卫：线性 `await script TITLE` + 身份日志 LIFT_LOG_MARKERS
//
// 红了意味着什么（判据，缺一条都不算）：
//   ① `await script TITLE` + `quit`（`--skip-logo`、headless）的子进程 **exit 0** —— 即
//      "等一屏 → 点一下"真的能自足写出来（不是只在 pure 用例里拼 IR）；
//   ② 它的 trace 里有 `node step1 enter` / `when script TITLE` / `task done` ——
//      线性写法的 step 真的进了状态机，且 `script` 条件是**电平**（已经在 TITLE 也成立）；
//   ③ 不带 `LIFT_LOG_MARKERS` 的那次**没有** `[marker] frame=` 行（默认关）；
//   ④ 带 `LIFT_LOG_MARKERS=1` 的那次**至少一行** `[marker] … name=` 且名字非空、**至少一行**不是
//      降级名（`id:0x…`）—— 这是"身份有发现路径"的机械判据（没有它只能靠猜名字）；
//   ⑤ ④ 那次用的图**没有** `script` 条件（`await screen changed`）⇒ 唯一的采样理由是环境变量
//      本身（⛔ 否则"日志非空"可能来自图自己的 `script` 条件，证明不了门控）。
//
// ★ 为什么不用 `--present window`：`--present headless` 是离屏呈现，不需要 OS 窗口 / 焦点 /
//   真实光标；GPU 不可用（raster 行不是 sdl-gpu）⇒ `LIFT_SKIP`(77)，那不是判据红。
// ★ 子进程输出走**文件重定向**（⛔ 不走管道：受限沙箱里捕获管道会 EPERM）。
// ★ 不写帧号 / 不断言耗时：TITLE 出现的位置随构建与机器漂，判据只问"到了没有"。
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "check.h"
#include "subprocess_skip.h"
#include "runtime/host_encoding.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/wait.h>  // WIFEXITED / WEXITSTATUS / WIFSIGNALED（POSIX 分支）
#endif

namespace {

namespace fs = std::filesystem;

struct ChildResult {
    int exit_code = -1;
    bool started = false;
};

std::string readAllBytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool contains(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

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
    // 每次 run 上限 8 分钟（两次总和 < CTest 的 TIMEOUT 1200）；卡住必须红，⛔ 不许挂死流水线。
    const DWORD waited = ::WaitForSingleObject(pi.hProcess, 480000u);
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

// 子进程继承本进程的环境（两条 `runChild` 都是）⇒ 在这里设 / 清。
void setEnvVar(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value == nullptr ? "" : value);
#else
    if (value == nullptr) unsetenv(name);
    else setenv(name, value, 1);
#endif
}

// 环境不满足（不是判据红）：GPU / 执行源 / 窗口。⛔ 不要匹配 `game-dir`（正常启动也打那行）。
// ★ "GPU 拒绝启动"**不在这里**：它由 `LIFT_SKIP_IF_NO_GPU` 单独判（要求本机实探无 GPU）。
bool environmentUnavailable(const std::string& log) {
    return contains(log, "窗口打开失败") || contains(log, "找不到执行源");
}

struct MarkerLineStats {
    long lines = 0;
    long empty_names = 0;
    long degraded = 0;
};

// 逐行读 `[marker] frame=… insn=… cur=… id=0x… name=…`：
//   ★ 判据只看"名字非空"与"有没有一行解析出了真名字"，⛔ 不锁具体名字 / 行数（那是读数）。
MarkerLineStats scanMarkerLines(const std::string& log) {
    MarkerLineStats s;
    const std::string prefix = "[marker] frame=";
    std::size_t pos = 0;
    while (pos <= log.size()) {
        const std::size_t nl = log.find('\n', pos);
        const std::string line = log.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (line.rfind(prefix, 0) == 0) {
            ++s.lines;
            const std::size_t at = line.find(" name=");
            const std::string name = at == std::string::npos ? std::string() : line.substr(at + 6);
            if (name.empty()) ++s.empty_names;
            if (contains(line, "（降级）")) ++s.degraded;
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return s;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace lift_test;
    g_current_case = "interaction-script-e2e";
#if !defined(_WIN32) && !defined(__APPLE__) && !defined(__linux__)
    (void)argc;
    (void)argv;
    LIFT_SKIP("本守卫只在 mac / linux / win 上起子进程");
#else
    const std::vector<std::string> args = lift::argvUtf8(argc, argv);
    if (args.size() < 4) {
        std::fprintf(stderr, "用法：lift-interaction-script-e2e <emulator-lift> <game-dir> <work-dir>\n");
        return 2;
    }
    const std::string exe_arg = args[1];
    const std::string game_dir = args[2];
    const std::string work_dir = args[3];
    std::error_code ec;
    fs::create_directories(work_dir, ec);

    // 图 A：线性写法（等一屏 → 收口）。`timeout` 是当前 step 的作用域——
    // 缺省 5000 ms 会在 TITLE 出现之前就判死（TITLE 要等 boot 过去）。
    const fs::path graph_a = fs::path(work_dir) / "await-title.graph";
    {
        std::ofstream out(graph_a, std::ios::binary | std::ios::trunc);
        out << "# 等 TITLE 那一屏，然后收口（线性写法）\n"
               "await script TITLE\n"
               "timeout 180000\n"
               "quit\n";
    }
    // 图 B：**没有** `script` 条件 —— 只为证明 `LIFT_LOG_MARKERS` 自己就打开了身份采样。
    const fs::path graph_b = fs::path(work_dir) / "markers.graph";
    {
        std::ofstream out(graph_b, std::ios::binary | std::ios::trunc);
        out << "await screen changed\n"
               "timeout 180000\n"
               "quit\n";
    }

    struct Arm {
        const char* tag;
        fs::path graph;
        bool log_markers;
    };
    const Arm arms[] = {
        {"await-script", graph_a, false},
        {"log-markers", graph_b, true},
    };

    for (const Arm& arm : arms) {
        const fs::path appdata = fs::path(work_dir) / (std::string("appdata-") + arm.tag);
        fs::remove_all(appdata, ec);
        fs::create_directories(appdata, ec);
        const fs::path log_path = fs::path(work_dir) / (std::string("e2e-") + arm.tag + ".log");
        const fs::path trace_path = fs::path(work_dir) / (std::string("e2e-") + arm.tag + ".trace");
        fs::remove(log_path, ec);
        fs::remove(trace_path, ec);

        // ★ 显式清掉 / 设置：父进程里残留的 `LIFT_LOG_MARKERS` 会让"默认关"那条假绿。
        if (arm.log_markers) setEnvVar("LIFT_LOG_MARKERS", "1");
        else setEnvVar("LIFT_LOG_MARKERS", nullptr);

        std::vector<std::string> child;
        child.push_back("--game-dir");
        child.push_back(game_dir);
        child.push_back("--appdata");
        child.push_back(appdata.string());
        child.push_back("--skip-logo");
        child.push_back("--audio");
        child.push_back("off");
        child.push_back("--present");
        child.push_back("headless");
        child.push_back("--interaction-graph");
        child.push_back(arm.graph.string());
        child.push_back("--interaction-trace");
        child.push_back(trace_path.string());
        child.push_back("--time-limit");
        child.push_back("300");

#if defined(_WIN32)
        std::vector<std::wstring> wargs;
        wargs.reserve(child.size());
        for (const std::string& a : child) wargs.push_back(lift::utf8ToWide(a));
        std::wstring wargs_joined;
        for (const std::wstring& a : wargs) wargs_joined += L" " + quoteArg(a);
        const ChildResult r = runChild(fs::path(lift::utf8ToWide(exe_arg)), wargs_joined, log_path);
#else
        const ChildResult r = runChild(fs::path(exe_arg), child, log_path);
#endif
        setEnvVar("LIFT_LOG_MARKERS", nullptr);

        if (!r.started) LIFT_SKIP("起不了 emulator-lift 子进程（exe 路径 / 沙箱限制？）");
        const std::string log = readAllBytes(log_path);
        if (log.empty()) LIFT_SKIP("子进程没写日志（工作目录 / 权限问题）");
        std::printf("interaction-script-e2e: arm=%s exit=%d trace=%s log=%s (%zu 字节)\n", arm.tag, r.exit_code,
                    trace_path.string().c_str(), log_path.string().c_str(), log.size());
        // ★ 无 GPU：子进程在跑 guest 之前就拒绝启动 ⇒ 环境缺失，SKIP 77（原因进输出）。
        LIFT_SKIP_IF_NO_GPU(r.exit_code, log);
        if (r.exit_code != 0 && environmentUnavailable(log)) {
            LIFT_SKIP("GPU / 执行源不可用 ⇒ 这条不是判据红");
        }
        static std::string case_name;
        case_name = std::string("interaction-script-e2e/") + arm.tag;
        g_current_case = case_name.c_str();
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(r.exit_code), 0u);  // ①

        const std::string trace = readAllBytes(trace_path);
        const MarkerLineStats marker = scanMarkerLines(log);
        std::printf("interaction-script-e2e: arm=%s marker_lines=%ld degraded=%ld\n", arm.tag, marker.lines,
                    marker.degraded);

        if (!arm.log_markers) {
            // ② 线性 step 真的进了状态机；`script` 是**电平**（已经在 TITLE 也命中）
            LIFT_CHECK(contains(trace, "node step1 enter"));
            LIFT_CHECK(contains(trace, "when script TITLE"));
            LIFT_CHECK(contains(trace, "task done"));
            // ③ 默认**关**：没有 `[marker]` 行（那条日志是 opt-in 的诊断）
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(marker.lines), 0u);
        } else {
            // ④ 身份发现路径：至少一行、名字非空、且至少一行解析出真名字（不是 `id:0x…` 降级名）
            LIFT_CHECK(marker.lines >= 1);
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(marker.empty_names), 0u);
            LIFT_CHECK(marker.degraded < marker.lines);
            // ⑤ 这一臂的图没有 `script` 条件 ⇒ 采样只因环境变量而开（门控的机械判据）
            LIFT_CHECK(!contains(trace, "when script "));
            LIFT_CHECK(contains(trace, "task done"));
        }
    }
    return finish("interaction-script-e2e");
#endif
}
