// emulator-lift · 宿主编码契约的守卫（Windows 缺陷回归）
//
// 判据（红了意味着什么）：
//   ① **源码 / 执行字符集**：`emulator-lift --help` 的字节里必须出现 main.cpp 那条中文说明的
//      **UTF-8** 编码。MSVC 少了目录级 `/utf-8` 时，窄字面量会按本地码页（936）落盘 ⇒ 这条红。
//   ② **argv**：带**日文**目录的 `--game-dir` 必须能正常启动（旧实现把 CRT 的 ANSI argv 当
//      UTF-8 解析 ⇒ `fs::path` 抛异常 ⇒ 退出码 3）。这里只跑 `--max-insns 1`，不需要游戏。
//   ③ **输出字节**：子进程输出（重定向到文件）必须是合法 UTF-8，且包含 `--game-dir` 的
//      UTF-8 回显 —— 保证"重定向时不做任何转码"的口径没有被破坏。
//
// ★ 控制台码页（SetConsoleOutputCP）这一半不在这里测：CTest 的子进程没有控制台，
//   `initConsoleUtf8()` 按设计不动（见 runtime/host_encoding.h）；它由人工在真控制台复核。
#include "runtime/host_encoding.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "check.h"

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

std::string readAllBytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::wstring quoteArg(const std::wstring& s) {
    // 只给本测试用的最小 quoting：路径不含引号；包一层双引号即可。
    return L"\"" + s + L"\"";
}

#if defined(_WIN32)
struct ChildResult {
    DWORD exit_code = 0xFFFFFFFFu;
    bool started = false;
};

ChildResult runChild(const fs::path& exe, const std::wstring& args, const fs::path& stdout_path,
                     const fs::path& stderr_path) {
    ChildResult r;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    const HANDLE h_out = ::CreateFileW(stdout_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL, nullptr);
    const HANDLE h_err = ::CreateFileW(stderr_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
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
    std::wstring cmd = quoteArg(exe.wstring()) + L" " + args;
    const BOOL ok = ::CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                     nullptr, &si, &pi);
    ::CloseHandle(h_out);
    ::CloseHandle(h_err);
    if (!ok) return r;
    r.started = true;
    ::WaitForSingleObject(pi.hProcess, INFINITE);
    ::GetExitCodeProcess(pi.hProcess, &r.exit_code);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return r;
}
#endif

}  // namespace

int main(int argc, char** argv) {
    // ---- ① 纯转换：宽 ↔ UTF-8（日文；非法 UTF-8 必须返回空）----
    {
        const std::string utf8 = lift::wideToUtf8(L"天結い");
        LIFT_CHECK(utf8 == "\xE5\xA4\xA9\xE7\xB5\x90\xE3\x81\x84");
        LIFT_CHECK(lift::utf8ToWide(utf8) == L"天結い");
        LIFT_CHECK(lift::utf8ToWide("\xFF").empty());
        LIFT_CHECK(lift::utf8ToWide("\xE5\xA4").empty());  // 截断
        LIFT_CHECK(lift::utf8ToWide("").empty());
    }

#if defined(_WIN32)
    // ★ 用 argvUtf8() 取自己的 argv（不是 CRT 的 ANSI argv）：否则构建树路径带非 ASCII 时，
    //   这个"守编码的测试"自己就先踩同一个缺陷。
    const std::vector<std::string> args = lift::argvUtf8(argc, argv);
    if (args.size() < 2) {
        LIFT_SKIP("需要 CTest 传 <emulator-lift.exe>（见 CMakeLists 的 host-encoding）");
    }
    const fs::path exe = fs::path(lift::utf8ToWide(args[1]));
    LIFT_CHECK(fs::exists(exe));

    std::error_code ec;
    // ★ 目录名故意带日文：这条用例同时覆盖"argv 是 UTF-8"与"输出回显是 UTF-8"。
    const std::string dir_name = "lift-host-encoding-" + std::to_string(::GetCurrentProcessId()) + "-天結";
    const fs::path root = fs::temp_directory_path(ec) / lift::utf8ToWide(dir_name);
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    LIFT_CHECK(!ec);

    // ---- ② 源码 / 执行字符集：--help 的字节必须是 UTF-8 ----
    {
        const fs::path out = root / "help.out";
        const fs::path err = root / "help.err";
        const ChildResult r = runChild(exe, L"--help", out, err);
        LIFT_CHECK(r.started);
        LIFT_CHECK_EQ_U32(r.exit_code, 0);
        const std::string bytes = readAllBytes(out);
        LIFT_CHECK(!bytes.empty());
        // main.cpp 的 usage() 第一行字面量：UTF-8 字节必须原样出现（936 落盘时不会有）
        LIFT_CHECK(bytes.find("emulator-lift \xE2\x80\x94\xE2\x80\x94 在 guest-state") != std::string::npos);
        LIFT_CHECK(!lift::utf8ToWide(bytes).empty());
    }

    // ---- ③ argv：日文 --game-dir 必须能启动（旧实现 exit 3）----
    {
        const fs::path out = root / "run.out";
        const fs::path err = root / "run.err";
        const std::wstring args = L"--game-dir " + quoteArg(root.wstring()) + L" --max-insns 1";
        const ChildResult r = runChild(exe, args, out, err);
        LIFT_CHECK(r.started);
        LIFT_CHECK_EQ_U32(r.exit_code, 0);
        const std::string bytes = readAllBytes(out);
        LIFT_CHECK(!bytes.empty());
        const std::string dir_utf8 = lift::wideToUtf8(root.c_str());
        LIFT_CHECK(bytes.find(dir_utf8) != std::string::npos);       // 回显了 UTF-8 路径
        LIFT_CHECK(!lift::utf8ToWide(bytes).empty());                // 整段输出合法 UTF-8
        // 子进程的错误输出也应可读（不要求为空：没有游戏目录时会打印 S6 警告，但 exit 仍是 0）
        const std::string err_bytes = readAllBytes(err);
        LIFT_CHECK(err_bytes.empty() || !lift::utf8ToWide(err_bytes).empty());
    }

    fs::remove_all(root, ec);

    // ---- ④ 控制台码页：真分配 / 复用一个控制台，确认 initConsoleUtf8 把输出码页切到 65001 ----
    //   ★ 这一条测的是"挂在控制台上"的分支；CTest 的 stdout 是管道，所以没有控制台时要自己分配。
    //   ★ 不 FreeConsole：若继承了调用者的控制台，FreeConsole 会把之后的 stdout/stderr 变成死句柄；
    //     若自己分配了，进程退出时自然销毁。码页用 restore + 手动恢复。
    {
        const UINT saved_cp = ::GetConsoleOutputCP();
        const bool had_console = saved_cp != 0;
        if (!had_console && !::AllocConsole()) {
            LIFT_SKIP("无法分配控制台（无窗口站？）");
        }
        if (!had_console) {
            if (const HWND w = ::GetConsoleWindow()) ::ShowWindow(w, SW_HIDE);
        }
        ::SetConsoleOutputCP(936);  // 先压到 GBK：避免"系统本来就是 65001"造成的假绿
        lift::initConsoleUtf8();
        LIFT_CHECK_EQ_U32(::GetConsoleOutputCP(), CP_UTF8);
        lift::restoreConsoleUtf8();
        if (saved_cp != 0) ::SetConsoleOutputCP(saved_cp);
    }
#else
    (void)argc;
    (void)argv;
#endif

    return lift_test::finish("host-encoding");
}
