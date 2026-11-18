// emulator-lift · `emulator-lift-gen` 的**输出字节恒为 ASCII** —— pure 运行时守卫
//
// 判据（红了意味着什么）：
//   ① `emulator-lift-gen` 在**不依赖任何资产**的几条入口路径上（`--help` / 参数错误 / skip），
//      stdout + stderr 的**每一个字节都 < 0x80**。红了 ⇒ 构建日志 / 管道 / 重定向消费者（编码
//      不可知）会看到非 ASCII 字节，按本地码页解码就是乱码 —— 那正是本单的缺陷面。
//   ② 每次输出的**退出码**不变（0 / 2），且关键字仍在大写 ASCII 里（`emulator-lift-gen` /
//      `unknown argument` / `skip`）⇒ ② 防"把文案改成空串/删掉"那种把 ① 变成恒真的改法。
//   ③ 每条路径都**真的产出字节**（≥1 B）⇒ 防"什么都不打"让 ① 恒真。
//
// ★ 为什么是**运行时**守卫（而不是扫源码的字面量）：
//   - 断言的是**字节**（本单的不变量），不是源码形状；源码里写 ASCII 但经由 `%s` 带进非 ASCII
//     时，扫描照样绿 —— 运行时守卫看得见。
//   - 不依赖资产：这几条路径不读 AGE.EXE / LFS / 安装树 ⇒ 顶层 `add_test` = **pure** 档
//     （AGENTS.md §12.4：判据 = 注册位置）。
//   - 覆盖 5 条不同的**产出口**：usage（stdout）、参数错误（stderr）、未知参数（stderr + usage）、
//     `--verify-split` / `--verify-ir-target` 的 skip（stdout）—— 每条都要**单独**验。
// ★ 覆盖边界：**不**覆盖 `--closure-gate` 成功路径（要 AGE.EXE，≈48 s，属 assets 档）与
//   `lift_gen_emit.cpp` 的错误分支（不在本单裁决的实现范围内）；见报告 §7。
// ★ 重定向必须是**文件**（⛔ 不用管道）：本仓有 `spawn EPERM` 先例（AGENTS.md §5.1 ①）。
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
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
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;

// 一条路径的读法：参数（ASCII）、期望退出码、期望出现在 stdout / stderr 里的 ASCII 子串
// （`nullptr` = 那条流不要求非空）。
struct Case {
    const char* name;
    std::vector<std::string> args;
    int want_exit;
    const char* want_in_stdout;  // nullptr = 不要求
    const char* want_in_stderr;  // nullptr = 不要求
};

#if defined(_WIN32)
std::wstring quoteWinArg(const std::wstring& s) {
    // 本用例的参数都是 ASCII 且不含引号 / 反斜杠结尾；仍按 CRT 规则加双引号，路径里有空格也安全。
    std::wstring out = L"\"";
    for (const wchar_t ch : s) {
        if (ch == L'"') {
            out += L'\\';
        }
        out += ch;
    }
    out += L'"';
    return out;
}

// 起子进程，stdout / stderr 都重定向到**文件**（⛔ 不是管道）。返回 false = 起不来（判据外）。
bool runRedirected(const std::string& exe, const std::vector<std::string>& args,
                   const fs::path& out_path, const fs::path& err_path, int* exit_code) {
    const std::wstring wexe = lift::utf8ToWide(exe);
    std::wstring cmd = quoteWinArg(wexe);
    for (const std::string& a : args) cmd += L" " + quoteWinArg(lift::utf8ToWide(a));

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    const std::wstring wout = out_path.wstring();
    const std::wstring werr = err_path.wstring();
    HANDLE hout = ::CreateFileW(wout.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hout == INVALID_HANDLE_VALUE) return false;
    HANDLE herr = ::CreateFileW(werr.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (herr == INVALID_HANDLE_VALUE) {
        ::CloseHandle(hout);
        return false;
    }
    // stdin 给 NUL：子进程不读它，但 `STARTF_USESTDHANDLES` 要求三个句柄都合法（⛔ 不传无效句柄）。
    HANDLE hnil = ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hout;
    si.hStdError = herr;
    si.hStdInput = hnil == INVALID_HANDLE_VALUE ? ::GetStdHandle(STD_INPUT_HANDLE) : hnil;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');
    const BOOL ok = ::CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                                     &si, &pi);
    ::CloseHandle(hout);
    ::CloseHandle(herr);
    if (hnil != INVALID_HANDLE_VALUE) ::CloseHandle(hnil);
    if (!ok) return false;
    ::WaitForSingleObject(pi.hProcess, 120000);
    DWORD code = 0;
    ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    *exit_code = static_cast<int>(code);
    return true;
}
#else
std::string quoteShellSingle(const std::string& s) {
    std::string out = "'";
    for (const char ch : s) {
        if (ch == '\'') {
            out += "'\\''";
        } else {
            out += ch;
        }
    }
    out += "'";
    return out;
}

// POSIX：`sh -c 'exe' 'args'... > out 2> err`。同样**只落文件**，不建管道。
bool runRedirected(const std::string& exe, const std::vector<std::string>& args,
                   const fs::path& out_path, const fs::path& err_path, int* exit_code) {
    std::string cmd = quoteShellSingle(exe);
    for (const std::string& a : args) cmd += " " + quoteShellSingle(a);
    cmd += " > " + quoteShellSingle(out_path.string()) + " 2> " + quoteShellSingle(err_path.string());
    const int rc = std::system(cmd.c_str());
    if (rc == -1) return false;
    if (WIFEXITED(rc)) {
        *exit_code = WEXITSTATUS(rc);
        return true;
    }
    return false;
}
#endif

bool readAll(const fs::path& p, std::vector<std::uint8_t>* out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out->clear();
    char buf[4096];
    while (f.read(buf, sizeof(buf)) || f.gcount() > 0) {
        out->insert(out->end(), buf, buf + static_cast<std::size_t>(f.gcount()));
    }
    return true;
}

bool contains(const std::vector<std::uint8_t>& bytes, const char* needle) {
    if (needle == nullptr) return true;
    const std::string hay(bytes.begin(), bytes.end());
    return hay.find(needle) != std::string::npos;
}

// 断言一整段输出全是 ASCII；红时点名**第一个**违规字节（偏移 + 值 + 32 B hex 窗口）。
void checkAscii(const char* stream_name, const std::vector<std::uint8_t>& bytes) {
    std::size_t bad = bytes.size();
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (bytes[i] >= 0x80) {
            bad = i;
            break;
        }
    }
    ++lift_test::g_checks;
    if (bad == bytes.size()) return;
    std::printf("  %s: first non-ASCII byte at offset %zu (0x%02X); window:", stream_name, bad,
                static_cast<unsigned>(bytes[bad]));
    const std::size_t lo = bad > 16 ? bad - 16 : 0;
    const std::size_t hi = bad + 16 < bytes.size() ? bad + 16 : bytes.size();
    for (std::size_t i = lo; i < hi; ++i) std::printf(" %02X", static_cast<unsigned>(bytes[i]));
    std::printf("\n");
    lift_test::failf(__FILE__, __LINE__, "%s contains a non-ASCII byte (offset %zu, 0x%02X of %zu B)",
                     stream_name, bad, static_cast<unsigned>(bytes[bad]), bytes.size());
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: lift-gen-ascii-output <path/to/emulator-lift-gen>\n");
        return 2;
    }
    const std::string gen = argv[1];
    std::error_code ec;
#if defined(_WIN32)
    const std::string pid = std::to_string(static_cast<unsigned long long>(::GetCurrentProcessId()));
#else
    const std::string pid = std::to_string(static_cast<long long>(::getpid()));
#endif
    const fs::path dir = fs::temp_directory_path(ec) / ("lift-gen-ascii-" + pid);
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    if (ec) {
        std::fprintf(stderr, "cannot create temp dir %s: %s\n", dir.string().c_str(), ec.message().c_str());
        return 2;
    }

    const std::vector<Case> cases = {
        // ① 帮助（真正的 usage 面；stdout）
        {"help", {"--help"}, 0, "emulator-lift-gen", nullptr},
        // ② 无参（必需参数缺失 ⇒ stderr 一行 + stdout usage）
        {"no-args", {}, 2, "emulator-lift-gen", "are required"},
        // ③ 未知参数（stderr 一行 + stdout usage）
        {"unknown-arg", {"--nope"}, 2, "emulator-lift-gen", "unknown argument"},
        // ④ 数值解析错误（只打 flag + 值，不碰路径）
        {"split-zero", {"--split", "0"}, 2, nullptr, "positive integer"},
        // ⑤ 独立模式：参数够、只是缺生成物 ⇒ skip；这条覆盖 `--verify-split` 的 stdout 文案
        {"verify-split-skip", {"--verify-split", dir.string()}, 0, "skip", nullptr},
        // ⑥ 独立模式：同上，`--verify-ir-target`
        {"verify-ir-target-skip", {"--verify-ir-target", dir.string(), "--split", "1"}, 0, "skip", nullptr},
        // ⑦ 审计模式缺 exe/roots（stderr）
        {"audit-missing", {"--jmptab-audit"}, 2, nullptr, "requires --exe and --roots"},
    };

    for (const Case& c : cases) {
        lift_test::g_current_case = c.name;
        const fs::path out_path = dir / (std::string(c.name) + ".out");
        const fs::path err_path = dir / (std::string(c.name) + ".err");
        int exit_code = -1;
        if (!runRedirected(gen, c.args, out_path, err_path, &exit_code)) {
            std::fprintf(stderr, "cannot spawn %s (case %s)\n", gen.c_str(), c.name);
            ++lift_test::g_failures;
            continue;
        }
        std::vector<std::uint8_t> out;
        std::vector<std::uint8_t> err;
        LIFT_CHECK(readAll(out_path, &out));
        LIFT_CHECK(readAll(err_path, &err));
        LIFT_CHECK_EQ_I64(exit_code, c.want_exit);
        if (exit_code != c.want_exit) {
            std::printf("  case %s: exit code %d (expected %d)\n", c.name, exit_code, c.want_exit);
        }
        // ③ 防恒真：这条路径必须真的产出字节（至少 stdout 或 stderr 之一）。
        LIFT_CHECK(!out.empty() || !err.empty());
        // ② 防"把文案删空"：期望的 ASCII 关键字必须出现在对应流里。
        LIFT_CHECK(contains(out, c.want_in_stdout));
        LIFT_CHECK(contains(err, c.want_in_stderr));
        // ① 本单的不变量：每一字节 < 0x80。
        checkAscii("stdout", out);
        checkAscii("stderr", err);
        std::printf("  case %s: exit=%d stdout=%zu B stderr=%zu B\n", c.name, exit_code, out.size(),
                    err.size());
    }

    fs::remove_all(dir, ec);
    return lift_test::finish("gen-ascii-output");
}
