// emulator-lift · 测试侧起子进程的唯一实现（见 `support/subprocess.h` 的判据）
//
// ★ 两个后端只共享**一条**参数构造路径：调用方给 UTF-8 token，Windows 侧转宽后交给
//   `tests/cmdline.h` 的 `joinCommandLine`（MSVC / `CommandLineToArgvW` 规则），POSIX 侧直接
//   `execv`（无 shell、无再解析）。
// ★ 捕获 = 两个临时文件（⛔ 管道，AGENTS §5.1），读在 `waitpid` / `WaitForSingleObject` 之后。
// ★ POSIX 的 "exec 失败" 不走管道握手：子进程把 `exec-failed` 写进第三个临时文件再 `_exit(127)`，
//   父进程收尸后读它 ⇒ `started=false` / `exit_code=-1`（与 Windows 的同步失败同语义）。
#include "support/subprocess.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "cmdline.h"
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

namespace lift_test {
namespace {

namespace fs = std::filesystem;

std::atomic<std::uint64_t> g_run_seq{0};

/** 一次 `runChild` 用的三个临时文件；析构时无论成功/失败都删。 */
struct TempFiles {
    fs::path out;
    fs::path err;
    fs::path status;

    ~TempFiles() {
        std::error_code ec;
        if (!out.empty()) fs::remove(out, ec);
        if (!err.empty()) fs::remove(err, ec);
        if (!status.empty()) fs::remove(status, ec);
    }
};

bool prepareTempFiles(TempFiles& t) {
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec);
    if (ec) return false;
    const std::uint64_t seq = g_run_seq.fetch_add(1, std::memory_order_relaxed);
    const std::string base =
        "lift-runchild-" + std::to_string(processId()) + "-" + std::to_string(seq);
    t.out = dir / (base + ".out");
    t.err = dir / (base + ".err");
    t.status = dir / (base + ".status");
    return true;
}

std::string readAllBytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

#if defined(_WIN32)

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string wideToUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr,
                                        nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

#else  // POSIX

// 子进程里只做 async-signal-safe 的事；这些系统调用都可能被信号打断 ⇒ EINTR 重试
// （⛔ 不把 EINTR 当失败：那会让一次信号变成 `_exit(127)` 的假红）。
int openRetry(const char* path, int flags, mode_t mode) {
    int fd = -1;
    do {
        fd = ::open(path, flags, mode);
    } while (fd < 0 && errno == EINTR);
    return fd;
}

int dup2Retry(int from, int to) {
    int rc = -1;
    do {
        rc = ::dup2(from, to);
    } while (rc < 0 && errno == EINTR);
    return rc;
}

int chdirRetry(const char* path) {
    int rc = -1;
    do {
        rc = ::chdir(path);
    } while (rc < 0 && errno == EINTR);
    return rc;
}

constexpr char kExecFailed[] = "exec-failed";

/** 子进程里的任何一步失败：把标记写进 status fd（若可写）再 `_exit(127)`。 */
[[noreturn]] void childFail(int status_fd) {
    if (status_fd >= 0) {
        const ssize_t ignored = ::write(status_fd, kExecFailed, sizeof(kExecFailed) - 1);
        (void)ignored;
    }
    ::_exit(127);
}

#endif  // _WIN32

}  // namespace

std::string ChildResult::text() const {
    std::string s = out;
    s += err;
#if defined(_WIN32)
    // ★ 唯一的归一：Windows 的 CRT 文本模式 / 产品 readout 用 `\r\n`；这条让断言只写一个 `\n`。
    //   其它平台不动（⛔ 不把"没有 CRLF"当成需要修补的东西）。
    std::string t;
    t.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\r' && i + 1 < s.size() && s[i + 1] == '\n') continue;
        t.push_back(s[i]);
    }
    s.swap(t);
#endif
    return s;
}

std::uint64_t processId() {
#if defined(_WIN32)
    return static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

std::string pathToUtf8(const fs::path& p) {
#if defined(_WIN32)
    return wideToUtf8(p.native());
#else
    return p.native();
#endif
}

ChildResult runChild(const fs::path& exe, const std::vector<std::string>& argv, const ChildOptions& opts) {
    ChildResult r;
    TempFiles files;
    if (!prepareTempFiles(files)) return r;  // 起不来 ⇒ started=false / -1（⛔ 不当 0）
    // ★ 先解析成绝对路径：POSIX 子进程会**先 chdir** 再 execv，相对路径在 chdir 之后就找不到 exe 了。
    //   `argv[0]` 仍是调用方给的那一串（与 exec 的目标不是一回事）。
    std::error_code abs_ec;
    const fs::path exe_abs = fs::absolute(exe, abs_ec);
    const fs::path& exe_target = abs_ec ? exe : exe_abs;

#if defined(_WIN32)

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    const HANDLE h_out = ::CreateFileW(files.out.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL, nullptr);
    const HANDLE h_err = ::CreateFileW(files.err.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h_out == INVALID_HANDLE_VALUE || h_err == INVALID_HANDLE_VALUE) {
        if (h_out != INVALID_HANDLE_VALUE) ::CloseHandle(h_out);
        if (h_err != INVALID_HANDLE_VALUE) ::CloseHandle(h_err);
        return r;
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = h_out;
    si.hStdError = h_err;
    PROCESS_INFORMATION pi{};
    // ★ 命令行拼装只有一份实现（`tests/cmdline.h`）：结尾反斜杠 / 内嵌引号 / 空串按 MSVC 规则转义。
    std::vector<std::wstring> wide_argv;
    wide_argv.reserve(argv.size() + 1);
    wide_argv.push_back(exe.native());
    for (const std::string& a : argv) wide_argv.push_back(utf8ToWide(a));
    std::wstring cmd = lift_test::joinCommandLine(wide_argv);
    const BOOL ok = ::CreateProcessW(exe_target.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                     nullptr, opts.cwd.empty() ? nullptr : opts.cwd.c_str(), &si, &pi);
    ::CloseHandle(h_out);
    ::CloseHandle(h_err);
    if (!ok) return r;  // exe 不存在 / 不可执行 ⇒ started=false / -1

    r.started = true;
    long long ms = opts.timeout.count();
    if (ms < 0) ms = 0;
    if (ms > 0x7FFFFFFFLL) ms = 0x7FFFFFFFLL;
    const DWORD waited = ::WaitForSingleObject(pi.hProcess, static_cast<DWORD>(ms));
    if (waited == WAIT_OBJECT_0) {
        DWORD code = 0xFFFFFFFFu;
        ::GetExitCodeProcess(pi.hProcess, &code);
        r.exit_code = static_cast<int>(code);
    } else {
        r.timed_out = true;
        ::TerminateProcess(pi.hProcess, 991u);
        ::WaitForSingleObject(pi.hProcess, 5000u);
        r.exit_code = 991;
    }
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    r.out = readAllBytes(files.out);
    r.err = readAllBytes(files.err);
    return r;

#else  // POSIX

    // 先做父进程侧可判的失败（⛔ 不 fork 一个必然 exec 失败的子进程再把 127 当成"跑完了"）。
    std::error_code ec;
    if (!fs::exists(exe_target, ec) || ec) return r;
    if (::access(exe_target.c_str(), X_OK) != 0) return r;

    // argv 在**父进程**里先建好（fork 之后子进程不再分配内存；指针在 fork 后仍有效）。
    std::vector<char*> child_argv;
    child_argv.reserve(argv.size() + 2);
    child_argv.push_back(const_cast<char*>(exe.c_str()));
    for (const std::string& a : argv) child_argv.push_back(const_cast<char*>(a.c_str()));
    child_argv.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) return r;  // 起不来 ⇒ started=false / -1
    if (pid == 0) {
        // ---- 子进程 ----
        // ★ 只做 async-signal-safe 的事；任何失败一律写标记 + `_exit(127)`。
        // ★ stdout/stderr 是**普通文件**（⛔ 不是管道）⇒ 这条路径不产生 SIGPIPE。
        const int fd_out = openRetry(files.out.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        const int fd_err = openRetry(files.err.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd_out < 0 || fd_err < 0) ::_exit(127);
        if (dup2Retry(fd_out, STDOUT_FILENO) < 0) ::_exit(127);
        if (dup2Retry(fd_err, STDERR_FILENO) < 0) ::_exit(127);
        const int status_fd =
            openRetry(files.status.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (!opts.cwd.empty() && chdirRetry(opts.cwd.c_str()) != 0) childFail(status_fd);
        // 不把父进程的其它 fd 带进 exec：3 以上全关（含 fd_out/fd_err 的原始副本；它们已 dup2 到 1/2）。
        // ★ 上界夹到 65536：Linux 的 `_SC_OPEN_MAX` 可能是 1M，而本测试进程不可能有那么高的 fd。
        const long open_max = ::sysconf(_SC_OPEN_MAX);
        const int close_upto = (open_max > 0 && open_max < 65536) ? static_cast<int>(open_max) : 65536;
        for (int fd = 3; fd < close_upto; ++fd) {
            if (fd != status_fd) ::close(fd);
        }
        for (;;) {  // execv 只在**失败**时返回；EINTR 也是失败的一种 ⇒ 重试
            ::execv(exe_target.c_str(), child_argv.data());
            if (errno != EINTR) break;
        }
        childFail(status_fd);  // exec 失败（ENOEXEC / 不可执行 ...）
    }
    // ---- 父进程 ----
    r.started = true;
    long long ms = opts.timeout.count();
    if (ms < 0) ms = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    int status = 0;
    bool reaped = false;
    for (;;) {
        const pid_t w = ::waitpid(pid, &status, WNOHANG);
        if (w == pid) {
            reaped = true;
            break;
        }
        if (w < 0) {
            if (errno == EINTR) continue;  // 被信号打断 ⇒ 重试（⛔ 不许把 EINTR 当"子进程没了"）
            break;                         // 其它错误：拿不到状态 ⇒ 下面记 -1（非 0）
        }
        // w == 0：还在跑。
        if (std::chrono::steady_clock::now() >= deadline) {
            r.timed_out = true;
            break;
        }
        // 轮询间隔 10 ms。★ 不用 `std::this_thread::sleep_for`：POSIX 上它可能把线程符号拖进链接行。
        const struct timespec poll = {0, 10 * 1000 * 1000};
        ::nanosleep(&poll, nullptr);
    }
    if (r.timed_out) {
        ::kill(pid, SIGKILL);
        for (;;) {  // 收尸同样要过 EINTR
            const pid_t w = ::waitpid(pid, &status, 0);
            if (w == pid) break;
            if (w < 0 && errno == EINTR) continue;
            break;
        }
        r.exit_code = 991;  // 与 Windows 分支的 `TerminateProcess(…, 991)` 对齐
    } else if (reaped) {
        if (WIFEXITED(status)) {
            r.exit_code = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            r.exit_code = 128 + WTERMSIG(status);  // 异常终止 ⇒ 非 0
        } else {
            r.exit_code = -1;
        }
    } else {
        r.exit_code = -1;  // waitpid 非 EINTR 失败（如 ECHILD）⇒ 非 0（⛔ 不当成 0）
    }
    if (reaped && r.exit_code == 127 && readAllBytes(files.status) == kExecFailed) {
        r.started = false;  // fork 成功但 exec 失败 ⇒ 与 Windows 的 CreateProcess 失败同语义
        r.exit_code = -1;
    }
    r.out = readAllBytes(files.out);
    r.err = readAllBytes(files.err);
    return r;

#endif  // _WIN32
}

}  // namespace lift_test
