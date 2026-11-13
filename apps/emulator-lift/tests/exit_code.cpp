// emulator-lift · guest `ExitProcess(n)` / `TerminateProcess(_, n)` 的**宿主进程退出码**守卫
// （REQ-01M77CMJGJ9EJVNPFS2TKMMHD1）
//
// 判据（红了意味着什么；缺一条都不算）：
//   ① 纯映射：`exitCodeForGuestExit(0) == 0`；`n != 0`（1 / 3 / 259 / 0xFFFFFFFF）⇒ **非 0**，
//      且**不等于 n**（⛔ 不把 guest 的任意 n 直接当宿主退出码；2 与 decode-error 同档）。
//   ② **真子进程**：自造最小 PE32（导入 `kernel32!ExitProcess`，入口 `push n; call [IAT]`），
//      跑 `emulator-lift --exe <PE>` 这条真实链路 ⇒
//        · `n=3` ⇒ 子进程退出码 **2**；stderr（**默认 warn 级**）里有一行含 `ExitProcess` 与
//          `exit_code=3`；stdout 的运行摘要 `detail` 含 `ExitProcess`（证明是 guest 真的调到了，
//          ⛔ 不是"载入失败所以非 0"那种假红）；
//        · `n=0` ⇒ 子进程退出码 **0**，同样有 `detail … ExitProcess`。
//   ③ `TerminateProcess(_, 3)` 走**同一口径**（warn + 宿主非 0 ⇒ 2）。
// ★ 为什么必须真子进程：缺陷的形态是"guest 的值只进了 `cpu.shim.exit_code`，从没参与进程退出码"。
//   只断言 accessor / 纯映射会漏掉"映射写了但 `main` 没接线"这一类半修 —— ② 穿过
//   PE 载入 → 导入绑定 → shim → run 停止 → main 最终 `return` 的整条链路。
// ★ 为什么执行源是**测试自己构造的最小 PE**：不碰 LFS / 游戏安装 ⇒ 顶层无条件注册 ⇒ pure 档。
//   子进程 cwd = 私有临时目录，显式给一个**不存在**的 `--lst`（否则默认候选的 sha256 与合成 PE
//   不符 ⇒ main 会在启动前 exit 1，那与"退出码分流"无关）。
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "check.h"
#include "runtime/machine.h"
#include "subprocess_skip.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "runtime/host_encoding.h"
#else
#include <ctime>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------------------
// 自造最小 PE32：两个节
//   .text  RVA 0x1000 / raw @0x400 —— 入口点（调用方给的机器码）
//   .rdata RVA 0x2000 / raw @0x600 —— 导入表：一个模块 `kernel32.dll`、一个导出 `api`
// 导入表布局（RVA）：
//   0x2000 descriptor（OFT 0x2028 / Name 0x2040 / FT 0x2060）；0x2014 起 20 B 终止符
//   0x2028 thunk 数组（Hint/Name RVA + 0）
//   0x2040 "kernel32.dll"
//   0x2060 IAT（bind() 会把 stub 写进这里；guest 用 `call dword ptr [IAT]`）
//   0x2080 hint(2 B) + "api\0"（⛔ 不能放 0x2050：`TerminateProcess` 17 B 会**压进 IAT**）
// ★ 与 `tests/cli_time_limit.cpp` 的 `minimalPe()` 同源（那里只要一条 nop）；这里多一节导入表。
// ---------------------------------------------------------------------------------------
std::vector<std::uint8_t> buildPe(const char* api, const std::vector<std::uint8_t>& code) {
    constexpr std::size_t kPeOff = 0x40;
    constexpr std::size_t kOpt = kPeOff + 24;
    constexpr std::size_t kOptSize = 0xE0;
    constexpr std::size_t kSec = kOpt + kOptSize;
    constexpr std::uint32_t kTextRva = 0x1000;
    constexpr std::uint32_t kRdataRva = 0x2000;
    constexpr std::size_t kTextRaw = 0x400;
    constexpr std::size_t kRdataRaw = 0x600;
    constexpr std::size_t kFileSize = 0x800;
    std::vector<std::uint8_t> b(kFileSize, 0);
    auto w16 = [&b](std::size_t off, std::uint16_t v) { std::memcpy(b.data() + off, &v, sizeof(v)); };
    auto w32 = [&b](std::size_t off, std::uint32_t v) { std::memcpy(b.data() + off, &v, sizeof(v)); };
    auto at = [](std::uint32_t rva) { return static_cast<std::size_t>(kRdataRaw + (rva - kRdataRva)); };

    b[0] = 'M';
    b[1] = 'Z';
    w32(0x3C, static_cast<std::uint32_t>(kPeOff));
    b[kPeOff + 0] = 'P';
    b[kPeOff + 1] = 'E';
    w16(kPeOff + 4, 0x014Cu);                              // Machine = i386
    w16(kPeOff + 6, 2u);                                   // NumberOfSections
    w16(kPeOff + 20, static_cast<std::uint16_t>(kOptSize));
    w16(kPeOff + 22, 0x0102u);                             // EXECUTABLE_IMAGE | 32BIT_MACHINE
    w16(kOpt + 0, 0x010Bu);                                // Magic = PE32
    w32(kOpt + 16, kTextRva);                              // AddressOfEntryPoint
    w32(kOpt + 28, 0x00400000u);                           // ImageBase
    w32(kOpt + 32, 0x1000u);                               // SectionAlignment
    w32(kOpt + 36, 0x200u);                                // FileAlignment
    w32(kOpt + 56, 0x3000u);                               // SizeOfImage
    w32(kOpt + 60, 0x400u);                                // SizeOfHeaders
    w32(kOpt + 72, 0x100000u);                             // SizeOfStackReserve
    w32(kOpt + 92, 3u);                                    // Subsystem = CONSOLE
    w32(kOpt + 96 + 8, kRdataRva);                         // DataDirectory[1] = import RVA
    w32(kOpt + 96 + 12, 0x100u);                           //                      size
    // .text
    std::memcpy(b.data() + kSec, ".text", 5);
    w32(kSec + 8, 0x100u);                                 // VirtualSize
    w32(kSec + 12, kTextRva);                              // VirtualAddress
    w32(kSec + 16, 0x200u);                                // SizeOfRawData
    w32(kSec + 20, kTextRaw);                              // PointerToRawData
    w32(kSec + 36, 0x60000020u);                           // CODE | EXECUTE | READ
    // .rdata
    std::memcpy(b.data() + kSec + 40, ".rdata", 6);
    w32(kSec + 40 + 8, 0x200u);
    w32(kSec + 40 + 12, kRdataRva);
    w32(kSec + 40 + 16, 0x200u);
    w32(kSec + 40 + 20, kRdataRaw);
    w32(kSec + 40 + 36, 0x40000040u);                      // INITIALIZED_DATA | READ
    // 入口代码
    if (code.size() > 0x200u) return {};
    std::memcpy(b.data() + kTextRaw, code.data(), code.size());
    // 导入描述符
    w32(at(0x2000), 0x2028u);                              // OriginalFirstThunk
    w32(at(0x200C), 0x2040u);                              // Name
    w32(at(0x2010), 0x2060u);                              // FirstThunk（IAT）
    w32(at(0x2028), 0x2080u);                              // thunk[0] = Hint/Name
    std::memcpy(b.data() + at(0x2040), "kernel32.dll", 13);
    b[at(0x2080)] = 0;                                     // hint = 0
    b[at(0x2081)] = 0;
    const std::size_t name_len = std::strlen(api);
    std::memcpy(b.data() + at(0x2082), api, name_len + 1);
    w32(at(0x2060), 0x2080u);                              // IAT 初值（bind 会覆盖）
    return b;
}

// `push imm32; call dword ptr [0x00402060]; int3` —— IAT 槽 = ImageBase + 0x2060。
std::vector<std::uint8_t> callOneArgImport(std::uint32_t arg) {
    std::vector<std::uint8_t> c;
    c.push_back(0x68);                                     // push imm32
    for (int i = 0; i < 4; ++i) c.push_back(static_cast<std::uint8_t>((arg >> (8 * i)) & 0xFFu));
    c.push_back(0xFF);                                     // call dword ptr [abs32]
    c.push_back(0x15);
    const std::uint32_t iat = 0x00402060u;
    for (int i = 0; i < 4; ++i) c.push_back(static_cast<std::uint8_t>((iat >> (8 * i)) & 0xFFu));
    c.push_back(0xCC);                                     // int3（正常路径到不了）
    return c;
}

// `push uExitCode; push hProcess(0); call [TerminateProcess IAT]; int3`
// 导出名不同 ⇒ Hint/Name 在同一位置、只是长度不同，IAT 槽地址不变（见 buildPe）。
std::vector<std::uint8_t> callTwoArgImport(std::uint32_t a, std::uint32_t b) {
    std::vector<std::uint8_t> c;
    c.push_back(0x68);
    for (int i = 0; i < 4; ++i) c.push_back(static_cast<std::uint8_t>((a >> (8 * i)) & 0xFFu));
    c.push_back(0x68);
    for (int i = 0; i < 4; ++i) c.push_back(static_cast<std::uint8_t>((b >> (8 * i)) & 0xFFu));
    c.push_back(0xFF);
    c.push_back(0x15);
    const std::uint32_t iat = 0x00402060u;
    for (int i = 0; i < 4; ++i) c.push_back(static_cast<std::uint8_t>((iat >> (8 * i)) & 0xFFu));
    c.push_back(0xCC);
    return c;
}

std::string readAllBytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool writeAllBytes(const fs::path& p, const std::vector<std::uint8_t>& b) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    return static_cast<bool>(out);
}

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

bool contains(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

struct ChildRun {
    bool started = false;
    bool timed_out = false;
    int exit_code = -1;
    std::string out;
    std::string err;
};

#if defined(_WIN32)
ChildRun runChild(const fs::path& exe, const std::vector<std::string>& args, const fs::path& cwd,
                  const fs::path& out_path, const fs::path& err_path) {
    ChildRun r;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    const HANDLE h_out = ::CreateFileW(out_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL, nullptr);
    const HANDLE h_err = ::CreateFileW(err_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h_out == INVALID_HANDLE_VALUE || h_err == INVALID_HANDLE_VALUE) {
        if (h_out != INVALID_HANDLE_VALUE) ::CloseHandle(h_out);
        if (h_err != INVALID_HANDLE_VALUE) ::CloseHandle(h_err);
        return r;
    }
    std::wstring joined;
    for (const std::string& a : args) joined += L" \"" + lift::utf8ToWide(a) + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = h_out;
    si.hStdError = h_err;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe.wstring() + L"\"" + joined;
    const BOOL ok = ::CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                     cwd.c_str(), &si, &pi);
    ::CloseHandle(h_out);
    ::CloseHandle(h_err);
    if (!ok) return r;
    r.started = true;
    // 60 s 上限：正常一次启动 + 两条指令 << 1 s；卡住必须**红**，⛔ 不许挂死流水线。
    const DWORD waited = ::WaitForSingleObject(pi.hProcess, 60000u);
    if (waited != WAIT_OBJECT_0) {
        r.timed_out = true;
        ::TerminateProcess(pi.hProcess, 991u);
        ::WaitForSingleObject(pi.hProcess, 5000u);
    }
    DWORD code = 0xFFFFFFFFu;
    ::GetExitCodeProcess(pi.hProcess, &code);
    r.exit_code = static_cast<int>(code);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    r.out = readAllBytes(out_path);
    r.err = readAllBytes(err_path);
    return r;
}
#else
ChildRun runChild(const fs::path& exe, const std::vector<std::string>& args, const fs::path& cwd,
                  const fs::path& out_path, const fs::path& err_path) {
    ChildRun r;
    const pid_t pid = ::fork();
    if (pid < 0) return r;
    if (pid == 0) {
        const int fo = ::open(out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        const int fe = ::open(err_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fo >= 0) ::dup2(fo, 1);
        if (fe >= 0) ::dup2(fe, 2);
        if (fo > 2) ::close(fo);
        if (fe > 2) ::close(fe);
        if (::chdir(cwd.c_str()) != 0) ::_exit(126);
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(exe.c_str()));
        for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        ::execv(exe.c_str(), argv.data());
        ::_exit(127);  // execv 失败（路径 / 权限）
    }
    r.started = true;
    int status = 0;
    bool reaped = false;
    // 60 s 上限（100 ms 一探）。超时 ⇒ SIGKILL + 收尸，⛔ 不留孤儿。
    for (int i = 0; i < 600; ++i) {
        const pid_t w = ::waitpid(pid, &status, WNOHANG);
        if (w == pid) {
            reaped = true;
            break;
        }
        if (w < 0) break;
        struct timespec ts{0, 100000000L};
        ::nanosleep(&ts, nullptr);
    }
    if (!reaped) {
        r.timed_out = true;
        ::kill(pid, SIGKILL);
        ::waitpid(pid, &status, 0);
    }
    if (WIFEXITED(status)) r.exit_code = WEXITSTATUS(status);
    r.out = readAllBytes(out_path);
    r.err = readAllBytes(err_path);
    return r;
}
#endif

// 一条子进程臂的公共形状：写 PE、跑、读回日志。
ChildRun runArm(const fs::path& emulator, const fs::path& root, const char* tag, const char* api,
                const std::vector<std::uint8_t>& code) {
    const fs::path pe = root / (std::string(tag) + ".exe");
    if (!writeAllBytes(pe, buildPe(api, code))) {
        return {};
    }
    const std::vector<std::string> args = {
        "--exe", pe.string(),
        // ★ 显式给一个**不存在**的 .lst：否则默认候选（仓库内 .lst）会被找到，sha256 与合成 PE
        //   不符 ⇒ main 在载入后拒绝启动（exit 1），与本单要测的退出码分流无关。
        "--lst", (root / "missing.lst").string(),
        "--game-dir", root.string(),
        "--audio", "off",
        "--present", "headless",
        "--max-insns", "10000",
        "--time-limit", "30",
    };
    return runChild(emulator, args, root, root / (std::string(tag) + ".out"),
                    root / (std::string(tag) + ".err"));
}

}  // namespace

int main(int argc, char** argv) {
    using namespace lift_test;

    if (argc < 2) LIFT_SKIP("需要 CTest 传 <emulator-lift>（见 CMakeLists 的 exit-code）");
    // ★ 绝对化：子进程会 `chdir` 到临时目录；相对路径（`./out/emulator-lift`）会在那里失效
    //   （实测 exit 127 = execv 失败）。CTest 传的是绝对路径，但守卫不该依赖这一点。
    std::error_code eca;
    const fs::path emulator = fs::absolute(fs::path(argv[1]), eca);
    LIFT_CHECK(!eca);
    LIFT_CHECK(fs::exists(emulator));

    // ---- ① 纯映射（投影的**唯一**定义）----
    g_current_case = "map";
    LIFT_CHECK_EQ_I64(lift::exitCodeForGuestExit(0u), 0);
    for (std::uint32_t n : {1u, 3u, 259u, 0xFFFFFFFFu}) {
        const int code = lift::exitCodeForGuestExit(n);
        LIFT_CHECK(code != 0);                                // 非 0 ⇒ 非 0（核心判据）
        LIFT_CHECK_EQ_I64(code, 2);                           // 与 decode-error 同档
        LIFT_CHECK(static_cast<std::uint32_t>(code) != n);    // ⛔ 不是透传 n
    }

    // ---- ② 真子进程：ExitProcess(3) ⇒ 非 0；ExitProcess(0) ⇒ 0 ----
    std::error_code ec;
#if defined(_WIN32)
    const fs::path root = fs::temp_directory_path(ec) /
                          ("lift-exit-code-" + std::to_string(::GetCurrentProcessId()));
#else
    const fs::path root = fs::temp_directory_path(ec) / ("lift-exit-code-" + std::to_string(::getpid()));
#endif
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    LIFT_CHECK(!ec);

    {
        g_current_case = "process/exit-3";
        const ChildRun r = runArm(emulator, root, "exit3", "ExitProcess", callOneArgImport(3u));
        LIFT_CHECK(r.started);
        LIFT_CHECK(!r.timed_out);
        // ★ 无 GPU 的机器 / `SDL_VIDEODRIVER=dummy`：子进程在**跑 guest 之前**就拒绝启动
        //   ⇒ 这条不是判据红，是环境缺失（原因写进输出）。真正参数错误仍红。
        LIFT_SKIP_IF_NO_GPU(r.exit_code, r.err);
        std::printf("exit-process(3): exit=%d\n", r.exit_code);
        LIFT_CHECK_EQ_I64(r.exit_code, 2);                    // ★ 宿主进程退出码（⛔ 不是 accessor）
        // 默认日志级别（warn）就能看见 n 本身（诊断所需），且证明走的是 ExitProcess。
        LIFT_CHECK(contains(r.err, "kernel32!ExitProcess"));
        LIFT_CHECK(contains(r.err, "exit_code=3"));
        LIFT_CHECK(contains(lineStartingWith(r.out, "detail"), "ExitProcess"));
        LIFT_CHECK(contains(lineStartingWith(r.out, "stop"), "stopped"));
    }
    {
        g_current_case = "process/exit-0";
        const ChildRun r = runArm(emulator, root, "exit0", "ExitProcess", callOneArgImport(0u));
        LIFT_CHECK(r.started);
        LIFT_CHECK(!r.timed_out);
        LIFT_SKIP_IF_NO_GPU(r.exit_code, r.err);
        std::printf("exit-process(0): exit=%d\n", r.exit_code);
        LIFT_CHECK_EQ_I64(r.exit_code, 0);                    // ★ n == 0 保持 0
        LIFT_CHECK(contains(lineStartingWith(r.out, "detail"), "ExitProcess"));
        LIFT_CHECK(contains(lineStartingWith(r.out, "stop"), "stopped"));
    }

    // ---- ③ TerminateProcess 同一口径（同一类"丢 code"的失效方式）----
    {
        g_current_case = "process/terminate-3";
        const ChildRun r = runArm(emulator, root, "term3", "TerminateProcess", callTwoArgImport(3u, 0u));
        LIFT_CHECK(r.started);
        LIFT_CHECK(!r.timed_out);
        LIFT_SKIP_IF_NO_GPU(r.exit_code, r.err);
        std::printf("terminate-process(3): exit=%d\n", r.exit_code);
        LIFT_CHECK_EQ_I64(r.exit_code, 2);
        LIFT_CHECK(contains(r.err, "kernel32!TerminateProcess"));
        LIFT_CHECK(contains(r.err, "exit_code=3"));
        LIFT_CHECK(contains(lineStartingWith(r.out, "detail"), "TerminateProcess"));
    }

    fs::remove_all(root, ec);
    return lift_test::finish("exit-code");
}
