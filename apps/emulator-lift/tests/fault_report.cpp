// emulator-lift · fault 归因（pure：不碰游戏 / LFS）
//
// ★ 盯的是 REQ-01M6JCBYBW8DV0920T28337ACF：fault 诊断必须按**异常地址**归因，
//   而不是按"最近一次 guest 访存"（`g_access` 在宿主 AV 时是陈旧值）。四条判据：
//   ① 纯函数 `inGuestSpace`：guest 空间内 ⇒ true；`0x1` ⇒ false；未预留 ⇒ false（两平台都跑）；
//   ② 宿主 AV（**先做一次 guest 访存把 `g_access` 弄脏**，再解引用 `0x1`）：
//      stderr 有 host fault 标记、**没有** `access     :` 行，退出码 = 4；
//   ③ 反向（同机制）：safe 模式读一个未提交页 ⇒ stderr **有** `access     :` 行、退出码 = 3
//      （证明按地址归因之后 guest 诊断没被弄坏）；
//   ④ win32：`hostPageSize()` 必须是 `GetSystemInfo().dwPageSize`（⛔ 不许硬编码 0x1000）。
// ⚠ ②③ 需要"子进程 + 收回它的 stderr"：POSIX 用 fork + 管道，Windows 用**同一个 exe 再启动一次**
//   （`--child-<mode>` 参数）+ 临时文件（写句柄经 STARTUPINFO 继承给子进程）。
//   ★ 两平台都**真跑**，没有 LIFT_SKIP；理由：fault 诊断以 `_Exit(3|4)` 结束进程 ——
//     在测试进程里直接触发会把测试自己带走，所以"触发 fault"的那一侧必须活在另一个进程里。
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "check.h"
#include "runtime/memory.h"

#if defined(_WIN32)
#include <cwchar>  // std::swprintf（check.h 已经带 <windows.h>：NOMINMAX / WIN32_LEAN_AND_MEAN）
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

// 常驻：fault handler 靠 memory.cpp 里的 `g_memory`（由 GuestMemory 构造时登记）找 guest 空间。
lift::GuestMemory* g_mem = nullptr;

constexpr std::uint32_t kCommitted = 0x00401000u;    // 本进程唯一提交的 guest 页
constexpr std::uint32_t kUncommitted = 0x20000000u;  // 离它足够远（16 KiB 宿主页也不会误提交）

// 把 `g_access` 弄脏：一次成功的 guest 写（EA = kCommitted，宽度 4，方向 WRITE）
void dirtyGuestAccess() { g_mem->wr32(kCommitted, 0xA5A5A5A5u); }

// 子进程：guest 访存之后制造一次**宿主** AV（0x1 在 guest 空间外）
[[noreturn]] void childFaultHost() {
    dirtyGuestAccess();
    volatile int* const p = reinterpret_cast<volatile int*>(0x1);
    const volatile int v = *p;  // 宿主 AV，异常地址 = 0x1 ⇒ 应判成 host fault（_Exit(4)）
    (void)v;
    std::_Exit(90);  // 没 fault ⇒ 用例红（父进程期望 4，看到 90）
}

// 子进程：safe 模式读未提交页 ⇒ 走 guestFault（有 access 行，_Exit(3)）
[[noreturn]] void childFaultGuest() {
    g_mem->setSafeMode(true);
    (void)g_mem->rd32(kUncommitted);
    std::_Exit(91);  // 没 fault ⇒ 用例红
}

struct ChildRun {
    int exit_code = -1;
    bool exited = false;
    std::string stderr_text;
};

#if defined(_WIN32)

// 子进程 = **同一个 exe** 加 `--child-<mode>` 再启动一次（Windows 没有 fork）。
// ★ 子进程的 stderr 写进临时文件（`CreateProcessW` 把该写句柄继承过去），父进程等它退出后读回来 ——
//   这与 POSIX 那条管道拿到的是**同一种证据**（子进程真的打了什么 + 真实退出码）。
ChildRun runChild(const wchar_t* mode) {
    ChildRun r;
    wchar_t exe[MAX_PATH] = {};
    const DWORD exe_len = ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (exe_len == 0 || exe_len >= MAX_PATH) {
        lift_test::failf(__FILE__, __LINE__, "GetModuleFileNameW 失败（err=%lu）",
                         static_cast<unsigned long>(::GetLastError()));
        return r;
    }

    wchar_t tmp_dir[MAX_PATH] = {};
    if (::GetTempPathW(MAX_PATH, tmp_dir) == 0) {
        lift_test::failf(__FILE__, __LINE__, "GetTempPathW 失败（err=%lu）",
                         static_cast<unsigned long>(::GetLastError()));
        return r;
    }
    wchar_t tmp_file[MAX_PATH] = {};
    if (::GetTempFileNameW(tmp_dir, L"lfr", 0, tmp_file) == 0) {
        lift_test::failf(__FILE__, __LINE__, "GetTempFileNameW 失败（err=%lu）",
                         static_cast<unsigned long>(::GetLastError()));
        return r;
    }

    // 写句柄要可继承；父进程在 CreateProcess 之后立刻关掉自己那份（否则子进程退出后文件仍被占着）。
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    const HANDLE out = ::CreateFileW(tmp_file, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) {
        lift_test::failf(__FILE__, __LINE__, "CreateFileW(stderr 临时文件) 失败（err=%lu）",
                         static_cast<unsigned long>(::GetLastError()));
        ::DeleteFileW(tmp_file);
        return r;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = out;  // 子进程正常路径不写 stdout；一并收进来不影响判据
    si.hStdError = out;

    wchar_t cmd[2 * MAX_PATH + 32] = {};
    std::swprintf(cmd, sizeof(cmd) / sizeof(cmd[0]), L"\"%ls\" --child-%ls", exe, mode);

    PROCESS_INFORMATION pi{};
    const BOOL ok = ::CreateProcessW(exe, cmd, nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
    ::CloseHandle(out);  // 子进程已继承一份
    if (!ok) {
        lift_test::failf(__FILE__, __LINE__, "CreateProcessW 失败（err=%lu，cmd=%ls）",
                         static_cast<unsigned long>(::GetLastError()), cmd);
        ::DeleteFileW(tmp_file);
        return r;
    }

    ::WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    r.exited = ::GetExitCodeProcess(pi.hProcess, &code) != 0;
    r.exit_code = r.exited ? static_cast<int>(code) : -1;
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);

    const HANDLE in = ::CreateFileW(tmp_file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (in != INVALID_HANDLE_VALUE) {
        char buf[4096];
        DWORD n = 0;
        while (::ReadFile(in, buf, static_cast<DWORD>(sizeof(buf)), &n, nullptr) != 0 && n > 0) {
            r.stderr_text.append(buf, static_cast<std::size_t>(n));
        }
        ::CloseHandle(in);
    }
    ::DeleteFileW(tmp_file);
    // 子进程的 stderr 原样留一份在测试输出里（红了才有用；绿了只是背景）
    std::printf("[child stderr]\n%s[/child stderr]\n", r.stderr_text.c_str());
    return r;
}

#else  // POSIX

// fork 一个子进程跑 fn()，把它写到 fd 2 的东西全部收回来（先读干净再 waitpid，避免管道写满互锁）。
ChildRun runChild(void (*fn)()) {
    ChildRun r;
    int fds[2] = {-1, -1};
    if (::pipe(fds) != 0) {
        lift_test::failf(__FILE__, __LINE__, "pipe() 失败");
        return r;
    }
    // ★ fork 前先把本进程的 stdio 缓冲清空：子进程里的 fault 诊断会 `fflush(nullptr)`，
    //   不清就会把父进程**继承过去的缓冲**再写一遍（同一条 [child stderr] 打印两次）。
    std::fflush(nullptr);
    const pid_t pid = ::fork();
    if (pid < 0) {
        lift_test::failf(__FILE__, __LINE__, "fork() 失败");
        ::close(fds[0]);
        ::close(fds[1]);
        return r;
    }
    if (pid == 0) {
        ::close(fds[0]);
        if (::dup2(fds[1], STDERR_FILENO) < 0) std::_Exit(93);
        ::close(fds[1]);
        fn();
        std::_Exit(92);  // fn 是 [[noreturn]]，不该走到这里
    }
    ::close(fds[1]);
    char buf[4096];
    ssize_t n = 0;
    while ((n = ::read(fds[0], buf, sizeof(buf))) > 0) r.stderr_text.append(buf, static_cast<std::size_t>(n));
    ::close(fds[0]);
    int status = 0;
    if (::waitpid(pid, &status, 0) != pid) {
        lift_test::failf(__FILE__, __LINE__, "waitpid() 失败");
        return r;
    }
    r.exited = WIFEXITED(status) != 0;
    r.exit_code = r.exited ? WEXITSTATUS(status) : -1;
    if (!r.exited) lift_test::failf(__FILE__, __LINE__, "子进程被信号杀死（signal %d）", WTERMSIG(status));
    // 子进程的 stderr 原样留一份在测试输出里（红了才有用；绿了只是背景）
    std::printf("[child stderr]\n%s[/child stderr]\n", r.stderr_text.c_str());
    return r;
}

#endif  // _WIN32

}  // namespace

int main(int argc, char** argv) {
    using namespace lift_test;

    const char* const mode = (argc > 1 && argv[1] != nullptr) ? argv[1] : "";

    lift::GuestMemory mem;
    g_mem = &mem;
    if (!mem.reserved()) {
        std::fprintf(stderr, "GuestMemory 预留失败：%s\n", mem.lastError());
        return 1;
    }
    if (!mem.commit(kCommitted, lift::GuestMemory::kPage, lift::RegionKind::Heap, "fault-report")) {
        std::fprintf(stderr, "提交 guest 页失败：%s\n", mem.lastError());
        return 1;
    }

    // ★ 子进程模式：真正触发 fault 的那一步必须活在**另一个进程**里（诊断会 `_Exit`）。
    if (std::strcmp(mode, "--child-host-av") == 0) childFaultHost();
    if (std::strcmp(mode, "--child-guest-fault") == 0) childFaultGuest();

    // ① 纯函数（两平台都跑）
    g_current_case = "in-guest-space-pure-function";
    const void* const guest_base = mem.host(0);
    const std::uintptr_t host_base = reinterpret_cast<std::uintptr_t>(guest_base);
    LIFT_CHECK(lift::inGuestSpace(guest_base, host_base));                                   // 空间起点
    LIFT_CHECK(lift::inGuestSpace(guest_base, host_base + 0x00401000u));                     // 中段
    LIFT_CHECK(lift::inGuestSpace(guest_base, host_base + lift::GuestMemory::kSpace - 1));   // 最后一个字节
    LIFT_CHECK(!lift::inGuestSpace(guest_base, host_base + lift::GuestMemory::kSpace));      // 右开区间
    LIFT_CHECK(!lift::inGuestSpace(guest_base, 0x1u));                                        // 宿主低地址 ⇒ host
    LIFT_CHECK(!lift::inGuestSpace(nullptr, host_base));                                      // 未预留 ⇒ 不归因 guest

#if defined(_WIN32)
    // ④ 宿主页大小以系统为准（⛔ 不许硬编码 0x1000）。
    //   ★ 本机 dwPageSize 恰好也是 0x1000 ⇒ "改回常量 0x1000"在这台机器上**测不出来**
    //     （这正是本单的已知边界）；这条守卫抓的是页面更大的宿主（16 KiB / 64 KiB）与用错来源
    //     （例如误用 dwAllocationGranularity = 64 KiB）。
    g_current_case = "host-page-size-from-system";
    SYSTEM_INFO si{};
    ::GetSystemInfo(&si);
    LIFT_CHECK_EQ_U32(lift::hostPageSize(), static_cast<std::uint32_t>(si.dwPageSize));

    // ② 宿主 AV：先弄脏 g_access，再解引用 0x1 ⇒ 必须是 host fault（不许报成 guest 访存）
    g_current_case = "host-av-attributed-to-host";
    const ChildRun host = runChild(L"host-av");
    LIFT_CHECK(host.exited);
    LIFT_CHECK_EQ_I64(host.exit_code, 4);  // 宿主 fault 的退出码（≠ guest 的 3）
    LIFT_CHECK(host.stderr_text.find("=== host fault") != std::string::npos);
    LIFT_CHECK(host.stderr_text.find("0x1") != std::string::npos);
    LIFT_CHECK(host.stderr_text.find("=== guest memory fault") == std::string::npos);
    LIFT_CHECK(host.stderr_text.find("access     :") == std::string::npos);  // ★ 陈旧 g_access 不许冒充本次 fault
    // 附注：那条"最近 guest 访存"确实被弄脏过（否则本用例证明不了"陈旧值不会冒充"）
    LIFT_CHECK(host.stderr_text.find("0x00401000") != std::string::npos);

    // ③ 反向：guest fault 仍是 guest fault（EA / 宽度 / 方向 + 退出码 3 都不回退）
    g_current_case = "guest-fault-still-guest";
    const ChildRun guest = runChild(L"guest-fault");
    LIFT_CHECK(guest.exited);
    LIFT_CHECK_EQ_I64(guest.exit_code, 3);
    LIFT_CHECK(guest.stderr_text.find("=== guest memory fault") != std::string::npos);
    LIFT_CHECK(guest.stderr_text.find("access     :") != std::string::npos);
    LIFT_CHECK(guest.stderr_text.find("READ 0x20000000  size=4") != std::string::npos);
    LIFT_CHECK(guest.stderr_text.find("=== host fault") == std::string::npos);

    return finish("fault-report");
#else
    // ② 宿主 AV：先弄脏 g_access，再解引用 0x1 ⇒ 必须是 host fault（不许报成 guest 访存）
    g_current_case = "host-av-attributed-to-host";
    const ChildRun host = runChild(&childFaultHost);
    LIFT_CHECK(host.exited);
    LIFT_CHECK_EQ_I64(host.exit_code, 4);  // 宿主 fault 的退出码（≠ guest 的 3）
    LIFT_CHECK(host.stderr_text.find("=== host fault") != std::string::npos);
    LIFT_CHECK(host.stderr_text.find("0x1") != std::string::npos);
    LIFT_CHECK(host.stderr_text.find("=== guest memory fault") == std::string::npos);
    LIFT_CHECK(host.stderr_text.find("access     :") == std::string::npos);  // ★ 陈旧 g_access 不许冒充本次 fault
    // 附注：那条"最近 guest 访存"确实被弄脏过（否则本用例证明不了"陈旧值不会冒充"）
    LIFT_CHECK(host.stderr_text.find("0x00401000") != std::string::npos);

    // ③ 反向：guest fault 仍是 guest fault（EA / 宽度 / 方向 + 退出码 3 都不回退）
    g_current_case = "guest-fault-still-guest";
    const ChildRun guest = runChild(&childFaultGuest);
    LIFT_CHECK(guest.exited);
    LIFT_CHECK_EQ_I64(guest.exit_code, 3);
    LIFT_CHECK(guest.stderr_text.find("=== guest memory fault") != std::string::npos);
    LIFT_CHECK(guest.stderr_text.find("access     :") != std::string::npos);
    LIFT_CHECK(guest.stderr_text.find("READ 0x20000000  size=4") != std::string::npos);
    LIFT_CHECK(guest.stderr_text.find("=== host fault") == std::string::npos);

    return finish("fault-report");
#endif
}
