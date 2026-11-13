// emulator-lift · 恢复判定（`--accept-recovery-dialog`）的计数守卫（**assets 档**）
//
// 判据（红了意味着什么）：
//   ① **默认档**（不传 flag）跑真 boot：`user32!MessageBoxA(...,00000034)` **一次都不被调**
//      —— 机械保证是 `0x004BA718 cmp eax,1` 之前的 EAX 被置 0 ⇒ 走 0x4BA856 干净返回
//      （⛔ 不是"靠不开窗"）。摘要必须有 `recovery  : bypassed @ 0x004BA718`。
//   ② **传 flag** 再跑同一份 `--appdata`：`$$AGE.TMP` 已由第一跑的 `0x4BB037` 写下 ⇒
//      `sub_454730` 能打开 ⇒ 走到 `0x4BA825` ⇒ **恰好 1 行** `...,00000034) -> 00000007`
//      （IDNO —— 答 YES(`0x4BA82B cmp eax,6`) 会删 marker + 返回 1 ⇒ 弹启动设置并退出）。
//      ⛔ 这条**只**用 headless：窗口档传 flag 后会真的等人，CTest 会挂到 TIMEOUT。
//   ③ 第二跑的摘要必须有 `messagebox: calls=1 last-type=0x00000034 last-result=7`。
//
// ★ 前提（找不到就**跳过**，⛔ 不算红）：`$$AGE.TMP` 的落点（`<base>\<set:RegFilePath>\`）里的
//   `set:RegFilePath` **未取证**（需求单 §未做）⇒ 这里**扫描** `<appdata>` 而不是拼路径；
//   扫不到 ⇒ `LIFT_SKIP(77)` 并打印原因。
//
// ★ 为什么跑两次同一个 appdata：不查 `set:RegFilePath`、不预制 marker —— 让引擎自己写。
//   第一跑 `--until ea 0x4BB082` 停在"写脏之后、建窗口之前"（`0x4BB037` 无条件 CREATE_ALWAYS）。
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "check.h"
#include "runtime/host_encoding.h"
#include "subprocess_skip.h"

#ifdef _WIN32
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

namespace fs = std::filesystem;

namespace {

constexpr const char* kMarkerName = "$$AGE.TMP";
// `0x4BB037` 写脏 → `0x4BB082` 建窗口；停在它之前。
constexpr const char* kUntilEa = "0x4BB082";

struct ChildResult {
    int exit_code = -1;
    bool started = false;
};

#if defined(_WIN32)
std::wstring quoteArg(const std::wstring& s) { return L"\"" + s + L"\""; }

ChildResult runChild(const fs::path& exe, const std::vector<std::string>& args, const fs::path& log_path) {
    ChildResult r;
    std::wstring joined;
    for (const std::string& a : args) joined += L" " + quoteArg(lift::utf8ToWide(a));
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
    std::wstring cmd = quoteArg(exe.wstring()) + joined;
    const BOOL ok = ::CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                     nullptr, &si, &pi);
    ::CloseHandle(h);
    if (!ok) return r;
    r.started = true;
    const DWORD waited = ::WaitForSingleObject(pi.hProcess, 600000u);
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
    if (rc == -1 || !fs::exists(log_path)) return r;
    r.started = true;
    r.exit_code = WIFEXITED(rc) ? WEXITSTATUS(rc) : (WIFSIGNALED(rc) ? 128 + WTERMSIG(rc) : 1);
    return r;
}
#endif

std::string readAllBytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool contains(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

// 逐行数"含齐所有 needles"的行数（⛔ 不用裸子串：别的 shim 的参数里也可能出现同一个十六进制数
// —— 实测 `HeapAlloc(...,00000034)` 就把裸子串判据骗过一次）。
std::size_t countShimLines(const std::string& hay, const std::vector<const char*>& needles) {
    std::size_t count = 0;
    std::size_t pos = 0;
    while (pos <= hay.size()) {
        std::size_t eol = hay.find('\n', pos);
        if (eol == std::string::npos) eol = hay.size();
        const std::string line = hay.substr(pos, eol - pos);
        bool all = true;
        for (const char* n : needles) {
            if (line.find(n) == std::string::npos) {
                all = false;
                break;
            }
        }
        if (all) ++count;
        if (eol == hay.size()) break;
        pos = eol + 1;
    }
    return count;
}

// `$$AGE.TMP` 的落点**未取证** ⇒ 递归扫（不是拼路径）。扫不到 = 用例前提不成立（SKIP，不是红）。
bool findMarker(const fs::path& root, fs::path* out) {
    std::error_code ec;
    if (!fs::exists(root, ec)) return false;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    if (ec) return false;
    const fs::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        if (it->path().filename() == kMarkerName) {
            *out = it->path();
            return true;
        }
    }
    return false;
}

// 环境不可用（不是判据红）：没有执行源 / 安装树不对。
// ★ GPU 拒绝启动**不在这里**：它由 `LIFT_SKIP_IF_NO_GPU` 单独判（要求本机实探无 GPU）。
bool environmentUnavailable(const std::string& log) {
    return contains(log, "找不到执行源") || contains(log, "SDL_Init(VIDEO)") ||
           contains(log, "--game-dir") || contains(log, "没有 SYS4INI.BIN");
}

}  // namespace

int main(int argc, char** argv) {
    using namespace lift_test;
    const std::vector<std::string> args = lift::argvUtf8(argc, argv);
    if (args.size() < 4) {
        std::fprintf(stderr,
                     "用法：lift-recovery-dialog <emulator-lift> <game-dir> <work-dir>\n");
        LIFT_CHECK(false);
        return finish("recovery-dialog");
    }
    g_current_case = "recovery-dialog";
    const fs::path exe = args[1];
    const std::string game_dir = args[2];
    const fs::path work = args[3];
    std::error_code ec;
    fs::remove_all(work, ec);  // 干净 appdata：marker 只能由**这一跑**写出来
    fs::create_directories(work / "appdata", ec);

    const fs::path appdata = work / "appdata";
    auto commonArgs = [&](bool accept_recovery) {
        std::vector<std::string> a = {
            "--present", "headless",
            "--skip-logo",
            "--audio", "off",
            "--game-dir", game_dir,
            "--appdata", appdata.string(),
            "--until", "ea", kUntilEa,
            "--trace-shims",
            "--time-limit", "1200",
        };
        if (accept_recovery) a.push_back("--accept-recovery-dialog");
        return a;
    };

    // ---- ① 默认档：恢复判定被绕过 ⇒ MessageBoxA 0 次 ----
    const fs::path log1 = work / "default.log";
    const ChildResult first = runChild(exe, commonArgs(false), log1);
    const std::string log_default = readAllBytes(log1);
    if (!first.started) {
        LIFT_SKIP("起不了子进程（emulator-lift 路径不对？）");
    }
    if (first.exit_code != 0) {
        // ★ 无 GPU：子进程在跑 guest 之前就拒绝启动 ⇒ 环境缺失，SKIP 77（原因进输出）。
        LIFT_SKIP_IF_NO_GPU(first.exit_code, log_default);
        if (environmentUnavailable(log_default)) LIFT_SKIP("环境不可用（没有执行源 / 安装树）");
        std::fprintf(stderr, "默认档子进程 exit %d；log 前 4000 字节：\n%.4000s\n", first.exit_code,
                     log_default.c_str());
        LIFT_CHECK(false);
        return finish("recovery-dialog");
    }
    LIFT_CHECK(contains(log_default, "stop      : until-ea"));
    LIFT_CHECK(contains(log_default, "recovery  : bypassed @ 0x004BA718"));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(
                          countShimLines(log_default, {"user32!MessageBoxA(", ",00000034)"})),
                      0);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(countShimLines(log_default, {"user32!MessageBoxA("})), 0);
    if (!contains(log_default, "messagebox: calls=")) {
        std::printf("默认档摘要没有 messagebox 行（calls=0 时有意不打）；这是期望形态。\n");
    }

    // ---- 前提：marker 真的落地（扫不到 ⇒ SKIP，⛔ 不算红）----
    fs::path marker;
    if (!findMarker(appdata, &marker)) {
        LIFT_SKIP("$$AGE.TMP 没出现：`set:RegFilePath` 未取证 ⇒ 本用例的前提不成立（⛔ 不算红）");
    }
    std::printf("marker: %s\n", marker.string().c_str());

    // ---- ② 传 flag 再跑同一份 appdata：恢复判定走真实路径 ⇒ 恰好 1 次 IDNO ----
    const fs::path log2 = work / "accepted.log";
    const ChildResult second = runChild(exe, commonArgs(true), log2);
    const std::string log_accepted = readAllBytes(log2);
    if (!second.started) {
        LIFT_CHECK(false);
        return finish("recovery-dialog");
    }
    if (second.exit_code != 0) {
        LIFT_SKIP_IF_NO_GPU(second.exit_code, log_accepted);
        if (environmentUnavailable(log_accepted)) LIFT_SKIP("环境不可用（没有执行源 / 安装树）");
        std::fprintf(stderr, "带 flag 子进程 exit %d；log 前 4000 字节：\n%.4000s\n", second.exit_code,
                     log_accepted.c_str());
        LIFT_CHECK(false);
        return finish("recovery-dialog");
    }
    LIFT_CHECK(contains(log_accepted, "recovery  : 未绕过（--accept-recovery-dialog"));
    LIFT_CHECK(contains(log_accepted, "trace-input recovery= 1"));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(countShimLines(
                          log_accepted, {"user32!MessageBoxA(", ",00000034) -> 00000007"})),
                      1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(countShimLines(log_accepted, {"user32!MessageBoxA("})), 1);
    LIFT_CHECK(contains(log_accepted, "messagebox: calls=1 last-type=0x00000034 last-result=7"));
    // 正文（引擎自己的错误串）在默认日志级别下可见（`REQ-01M75S724...` 判据②的兄弟证据）。
    LIFT_CHECK(contains(log_accepted, "[warn] [shim] user32!MessageBoxA: type=0x00000034"));
    return finish("recovery-dialog");
}
