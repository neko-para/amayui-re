// emulator-lift · `tests/support/subprocess.{h,cpp}` 的自检（CTest `support-child-proc`）
//
// ★ 这个二进制**自己既是测试又是子进程**：`argv[1] == "--child"` 时走子进程模式
//   （打印 argv / cwd / 一行 stdout / 一行 stderr，按 `--exit=N` 退出或 `--sleep-ms=N` 睡眠）。
// ★ 判据（红了意味着什么）：
//   ① argv 逐 token 还原（空格 / `天結` / 尾 `\` / 内嵌 `"` / 空串）——
//      红了 ⇒ 子进程收到的开关与调用方给的不是同一组（Windows 上是 `joinCommandLine` 转义坏了）。
//   ② stdout / stderr 落在**两个文件**（⛔ 不合并、⛔ 不是管道）⇒ 两流互不污染。
//   ③ `exit 7` → 7；POSIX 信号 → `128+sig`；超时 → `timed_out` + 991 且 200 ms 预算下 <2 s 返回。
//   ④ exe 不存在 ⇒ `started=false` + `exit_code=-1`（⛔ 不当 0）。
//   ⑤ `opts.cwd` 真的生效（子进程打印的 cwd 与要求的一致）。
// ★ 顶层无条件注册 ⇒ **pure** 档（判据 = 注册位置，AGENTS §12.4）；不碰 GPU / 资产 / 游戏安装。
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "check.h"
#include "support/subprocess.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <shellapi.h>
#else
#include <signal.h>
#include <time.h>
#endif

namespace {

namespace fs = std::filesystem;

/** 本二进制的路径：Windows 取模块路径（`argv[0]` 的窄编码会吃掉非 ASCII），POSIX 用 `argv[0]`。 */
fs::path selfPath(const std::vector<std::string>& args) {
#if defined(_WIN32)
    std::wstring buf(32768, L'\0');
    const DWORD n = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n > 0 && n < buf.size()) {
        buf.resize(static_cast<std::size_t>(n));
        return fs::path(buf);
    }
#endif
    return fs::path(args.empty() ? "" : args[0]);
}

#if defined(_WIN32)

std::string wideToUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr,
                                        nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

/** Windows：子进程的 argv 也走 `CommandLineToArgvW`（与产品 `host_encoding.cpp` 同一套规则）。 */
std::vector<std::string> collectArgv() {
    std::vector<std::string> out;
    int wargc = 0;
    LPWSTR* wargv = ::CommandLineToArgvW(::GetCommandLineW(), &wargc);
    if (wargv == nullptr) return out;
    out.reserve(static_cast<std::size_t>(wargc));
    for (int i = 0; i < wargc; ++i) out.push_back(wideToUtf8(wargv[i]));
    ::LocalFree(wargv);
    return out;
}

#else

std::vector<std::string> collectArgv(int argc, char** argv) {
    std::vector<std::string> out;
    out.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) out.emplace_back(argv[i] == nullptr ? "" : argv[i]);
    return out;
}

#endif

void sleepMs(long long ms) {
#if defined(_WIN32)
    ::Sleep(static_cast<DWORD>(ms));
#else
    struct timespec req = {time_t(ms / 1000), static_cast<long>((ms % 1000) * 1000 * 1000)};
    while (::nanosleep(&req, &req) != 0 && errno == EINTR) {
    }
#endif
}

// ---------------------------------------------------------------------------------------
// 子进程模式
// ---------------------------------------------------------------------------------------
int runAsChild(const std::vector<std::string>& args) {
    std::printf("argc=%zu\n", args.size());
    for (std::size_t i = 0; i < args.size(); ++i) std::printf("arg[%zu]=%s\n", i, args[i].c_str());

    bool print_cwd = false;
    bool sigterm = false;
    int exit_code = 0;
    long long sleep_ms = 0;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--print-cwd") {
            print_cwd = true;
        } else if (a == "--sigterm") {
            sigterm = true;
        } else if (a.rfind("--exit=", 0) == 0) {
            exit_code = std::atoi(a.c_str() + 7);
        } else if (a.rfind("--sleep-ms=", 0) == 0) {
            sleep_ms = std::strtoll(a.c_str() + 11, nullptr, 10);
        }
    }
    if (print_cwd) std::printf("cwd=%s\n", lift_test::pathToUtf8(fs::current_path()).c_str());
    std::printf("child-stdout\n");
    std::fprintf(stderr, "child-stderr\n");
    // ★ 睡之前必须 flush：stdout 落到普通文件是块缓冲，不 flush 的话父进程在超时路径上读到空文件。
    std::fflush(stdout);
    std::fflush(stderr);
    if (sleep_ms > 0) sleepMs(sleep_ms);
    if (sigterm) {
#if defined(_WIN32)
        // Windows 没有 POSIX 信号；子判据在测试侧跳过（见 runSelfTest 的说明）。
        return exit_code;
#else
        ::raise(SIGTERM);
#endif
    }
    return exit_code;
}

// ---------------------------------------------------------------------------------------
// 自检
// ---------------------------------------------------------------------------------------

/** `arg[i]=<value>` 行 → 按 i 顺序还原（i 必须从 0 连续）。 */
std::vector<std::string> parseArgLines(const std::string& out) {
    std::vector<std::string> got;
    std::size_t pos = 0;
    while (pos <= out.size()) {
        const std::size_t nl = out.find('\n', pos);
        const std::string line = out.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (line.rfind("arg[", 0) == 0) {
            const std::size_t close = line.find("]=");
            if (close == std::string::npos) return {};
            const std::string index = line.substr(4, close - 4);
            char* end = nullptr;
            const long n = std::strtol(index.c_str(), &end, 10);
            if (end == nullptr || *end != '\0' || n != static_cast<long>(got.size())) return {};
            got.push_back(line.substr(close + 2));
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return got;
}

/** 第一行以 `prefix` 开头的行（没有 ⇒ 空串）。 */
std::string firstLineWithPrefix(const std::string& text, const std::string& prefix) {
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

int runSelfTest(const std::vector<std::string>& args) {
    using namespace lift_test;
    const fs::path self = selfPath(args);

    std::error_code ec;
    const fs::path root = fs::temp_directory_path(ec) / ("lift-support-child-proc-" + std::to_string(processId()));
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    LIFT_CHECK(!ec);
    const fs::path work = root / "work";
    fs::create_directories(work, ec);
    LIFT_CHECK(!ec);

    // ---- ① argv 逐 token + ② 分文件 + ⑤ cwd ----
    g_current_case = "support/argv-and-capture";
    {
        // ★ 五个刁钻 token：空格 / 日文 / 结尾反斜杠（Windows 临时目录的真实形状）/ 内嵌引号 / 空串。
        const std::vector<std::string> expected = {
            pathToUtf8(self), "--child", "a b", "天結", "tail\\", "in\"side", "", "--print-cwd",
        };
        ChildOptions opts;
        opts.cwd = work;
        const std::vector<std::string> child_argv(expected.begin() + 1, expected.end());
        const ChildResult r = runChild(self, child_argv, opts);
        LIFT_CHECK(r.started);
        LIFT_CHECK(!r.timed_out);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(r.exit_code), 0u);

        // ② stdout / stderr 分文件（两流互不出现）。
        LIFT_CHECK(r.out.find("child-stdout") != std::string::npos);
        LIFT_CHECK(r.err.find("child-stderr") != std::string::npos);
        LIFT_CHECK(r.out.find("child-stderr") == std::string::npos);
        LIFT_CHECK(r.err.find("child-stdout") == std::string::npos);
        // `text()` = out + err（两段都在；⛔ 不是把 stderr 并进 stdout 文件）。
        LIFT_CHECK(r.text().find("child-stdout") != std::string::npos);
        LIFT_CHECK(r.text().find("child-stderr") != std::string::npos);

        // ① 逐 token 相等（含 `argc`）。
        LIFT_CHECK(r.out.find("argc=" + std::to_string(expected.size())) != std::string::npos);
        const std::vector<std::string> got = parseArgLines(r.out);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(got.size()), static_cast<std::int64_t>(expected.size()));
        if (got.size() == expected.size()) {
            for (std::size_t i = 0; i < expected.size(); ++i) {
                LIFT_CHECK(got[i] == expected[i]);
                if (got[i] != expected[i]) {
                    lift_test::failf(__FILE__, __LINE__, "argv[%zu] 期望 `%s`，实际 `%s`", i,
                                     expected[i].c_str(), got[i].c_str());
                }
            }
        }

        // ⑤ cwd 生效：子进程打印的路径必须解析回 `work`（mac 的 /var → /private/var 也归一）。
        const std::string cwd_line = firstLineWithPrefix(r.out, "cwd=");
        LIFT_CHECK(!cwd_line.empty());
        if (!cwd_line.empty()) {
            std::error_code cec;
            const fs::path observed = fs::canonical(fs::path(cwd_line.substr(4)), cec);
            LIFT_CHECK(!cec);
            LIFT_CHECK(observed == fs::canonical(work, cec));
        }
    }

    // ---- ③ 退出码：`exit 7` → 7 ----
    g_current_case = "support/exit-code";
    {
        ChildOptions opts;
        opts.cwd = work;
        const ChildResult r = runChild(self, {"--child", "--exit=7"}, opts);
        LIFT_CHECK(r.started);
        LIFT_CHECK(!r.timed_out);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(r.exit_code), 7u);
    }

    // ---- ③ 信号 → `128+sig`（POSIX；Windows 无 POSIX 信号，子判据跳过并在日志里说明）----
    g_current_case = "support/signal-exit";
#if defined(_WIN32)
    std::printf("support-child-proc: 跳过信号子判据 —— Windows 无 WIFSIGNALED，TerminateProcess 只产生退出码\n");
#else
    {
        ChildOptions opts;
        opts.cwd = work;
        const ChildResult r = runChild(self, {"--child", "--sigterm"}, opts);
        LIFT_CHECK(r.started);
        LIFT_CHECK(!r.timed_out);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(r.exit_code), static_cast<std::uint32_t>(128 + SIGTERM));
    }
#endif

    // ---- ③ 超时：5 s 睡眠 + 200 ms 预算 ⇒ `timed_out` / 991 且 <2 s 返回 ----
    g_current_case = "support/timeout";
    {
        ChildOptions opts;
        opts.cwd = work;
        opts.timeout = std::chrono::milliseconds(200);
        const auto t0 = std::chrono::steady_clock::now();
        const ChildResult r = runChild(self, {"--child", "--sleep-ms=5000"}, opts);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - t0)
                                 .count();
        LIFT_CHECK(r.started);
        LIFT_CHECK(r.timed_out);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(r.exit_code), 991u);
        LIFT_CHECK(elapsed < 2000);
        std::printf("support-child-proc: timeout elapsed=%lld ms\n", static_cast<long long>(elapsed));
    }

    // ---- ④ exe 不存在 ⇒ `started=false` / -1（⛔ 不当 0、不崩）----
    g_current_case = "support/missing-exe";
    {
        ChildOptions opts;
        opts.cwd = work;
        opts.timeout = std::chrono::milliseconds(1000);
        const ChildResult r = runChild(root / "no-such-child-binary", {"--child"}, opts);
        LIFT_CHECK(!r.started);
        LIFT_CHECK(!r.timed_out);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(r.exit_code), -1);
    }

    fs::remove_all(root, ec);
    return lift_test::finish("support-child-proc");
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    (void)argc;
    (void)argv;
    const std::vector<std::string> args = collectArgv();
#else
    const std::vector<std::string> args = collectArgv(argc, argv);
#endif
    if (args.size() > 1 && args[1] == "--child") return runAsChild(args);
    return runSelfTest(args);
}
