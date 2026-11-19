// emulator-lift · **AOT 臂令牌摘要**的 pure 守卫（顶层无条件注册 ⇒ pure 档；真子进程 + 真 run）
//
// 判据（红了意味着什么）：
//   ① `--profile --max-frames 1` 的 stdout 里**恰好一行** `token     : violations=<n> max-concurrent=<m>`
//      （与解释器 `Machine::dumpRunSummary` 同形：`first=<site>` 只在 n != 0 时出现，m >= 1）。
//      红了意味着：AOT 不再 `guestToken().arm(...)`（相位 0 关着 ⇒ 一行不打），或摘要那行被删 /
//      被无条件打 / 形状与解释器分叉。
//   ② **缺省档**（不给 `--profile`、不设 `LIFT_AUDIO_TRACE`）的 stdout 里**没有** `token     :`，
//      也没有 Phase-0 的四行（`tokenhold` / `holdhist` / `waithist` / `shimmax`），而 `frames    : 1`
//      在场（证明这一跑真的跑完并打了摘要）。
//      红了意味着：那条读数越过了 `guestToken().phase0Enabled()` 闸门 ⇒ AOT 缺省档 stdout 不再逐字
//      不变（`AGENTS.md` §12.8 第 3 条；音频 S0–S4 那轮最硬的判据）。
//
// ★ 为什么是**真 run**：`arm()` 是运行期调用、`phase0Enabled()` 是运行期开关、`printRunSummary` 是
//   运行期打印 —— 纯夹具只能钉 `GuestToken` 自己（`tests/guest_token.cpp` 已钉）。
//   代价：mac arm64 / RelWithDebInfo 两次 run ≈ 3 s（2026-11-19 实测）。
// ★ 输入（执行源 / 安装树）假定在场，缺了 `LIFT_SKIP`（77）。
// ★ 子进程输出**重定向到文件**（⛔ 不走管道：受限沙箱里管道会 EPERM；与 `tests/aot_quit_exit.cpp`
//   的 `runChild` 同一口径）。
// ★ **本用例不断言 `violations==0`**：2026-11-19 实测 AOT 真跑 `violations=2 first=GuestHeap::free`
//   （`EnumFontFamiliesExA` 的续延在令牌外 free guest 堆；AOT 的块循环不像解释器那样每块持令牌）。
//   那是**另一个缺陷**（`REQ-01M7SAHRTGZ760B35CQXASXT0R`），⛔ 不许为了让它变绿而改弱这里或把它调成 0；
//   本用例只钉"读数存在、形状与解释器同形、缺省档不出现"。
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
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

std::size_t countOf(const std::string& hay, const std::string& needle) {
    std::size_t n = 0;
    for (std::size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + needle.size())) ++n;
    return n;
}

#if !defined(_WIN32)
std::string shq(const std::string& s) {
    std::string out = "'";
    for (const char c : s) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    out += "'";
    return out;
}
#endif

// 起子进程并等它结束；stdout / stderr 重定向到两个文件。返回进程退出码（拿不到 ⇒ -1）。
int runChild(const fs::path& exe, const std::vector<std::string>& args, const fs::path& cwd, const fs::path& out_path,
             const fs::path& err_path) {
#if defined(_WIN32)
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
        return -1;
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = h_out;
    si.hStdError = h_err;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = lift_test::quoteArg(exe.wstring());
    for (const std::string& a : args) cmd += L" " + lift_test::quoteArg(std::wstring(a.begin(), a.end()));
    const BOOL ok = ::CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                     cwd.c_str(), &si, &pi);
    ::CloseHandle(h_out);
    ::CloseHandle(h_err);
    if (!ok) return -1;
    // ★ 120 s 上限：两次 run 在本机各 ~1.3 s；卡住必须**红**（CTest 侧还有 TIMEOUT 兜底）。
    const DWORD waited = ::WaitForSingleObject(pi.hProcess, 120000u);
    if (waited != WAIT_OBJECT_0) {
        ::TerminateProcess(pi.hProcess, 991u);
        ::WaitForSingleObject(pi.hProcess, 5000u);
    }
    DWORD code = 0xFFFFFFFFu;
    ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return static_cast<int>(code);
#else
    std::string cmd = shq(exe.string());
    for (const std::string& a : args) cmd += " " + shq(a);
    cmd += " > " + shq(out_path.string()) + " 2> " + shq(err_path.string());
    const std::string wrapped = "cd " + shq(cwd.string()) + " && " + cmd;
    const int status = std::system(wrapped.c_str());
    if (status == -1) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
#endif
}

/** 从 stdout 取 `token     : …` 那一行（没有 ⇒ 空串）。⛔ 不用前缀匹配任意行：只认这一条。 */
std::string tokenLine(const std::string& out) {
    const std::string prefix = "token     : ";
    std::size_t pos = 0;
    while (pos <= out.size()) {
        const std::size_t nl = out.find('\n', pos);
        const std::string line = out.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (line.compare(0, prefix.size(), prefix) == 0) return line;
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return {};
}

/** 取 `name=` 后面的十进制整数；没有 / 非法 ⇒ -1。 */
long long fieldValue(const std::string& line, const std::string& name) {
    const std::size_t at = line.find(name);
    if (at == std::string::npos) return -1;
    std::size_t i = at + name.size();
    long long v = 0;
    bool any = false;
    while (i < line.size() && line[i] >= '0' && line[i] <= '9') {
        v = v * 10 + (line[i] - '0');
        any = true;
        ++i;
    }
    return any ? v : -1;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace lift_test;

    if (argc < 4) {
        LIFT_SKIP("需要 CTest 传 <emulator-lift-aot> <game-dir> <corpus/assets/age/AGE.EXE__dumped.sectfix.EXE>"
                  "（见 CMakeLists）");
    }
    const fs::path aot_exe = argv[1];
    const fs::path game_dir = argv[2];
    const fs::path exec_pe = argv[3];

    std::error_code ec;
    if (!fs::exists(aot_exe, ec)) LIFT_SKIP("emulator-lift-aot 不在场");
    if (!fs::is_directory(game_dir, ec)) LIFT_SKIP("安装树（--game-dir）不在场");
    if (!fs::exists(exec_pe, ec)) LIFT_SKIP("执行源（corpus/assets/age/AGE.EXE__dumped.sectfix.EXE）不在场");

    const fs::path root = fs::temp_directory_path(ec) /
                          ("lift-aot-token-summary-" + std::to_string(
#if defined(_WIN32)
                                                              ::GetCurrentProcessId()
#else
                                                              ::getpid()
#endif
                                                              ));
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    LIFT_CHECK(!ec);

    // ---- ① `--profile`：令牌摘要行必须出现，且与解释器同形 ----
    g_current_case = "aot-token-summary/profile-line";
    {
        const fs::path appdata = root / "appdata-profile";
        fs::create_directories(appdata, ec);
        LIFT_CHECK(!ec);
        const std::vector<std::string> args = {"--present", "headless", "--audio", "off",
                                               "--exe", exec_pe.string(), "--max-frames", "1",
                                               "--profile", "--game-dir", game_dir.string(),
                                               "--appdata", appdata.string(), "--time-limit", "120"};
        const fs::path out = root / "profile.out";
        const fs::path err = root / "profile.err";
        const int code = runChild(aot_exe, args, root, out, err);
        const std::string stdout_text = readAll(out);
        if (code != 0) {
            const std::string tail = stdout_text.size() > 4000 ? stdout_text.substr(stdout_text.size() - 4000) : stdout_text;
            failf(__FILE__, __LINE__, "--profile 的短跑应 exit 0，实际 exit=%d\n--- stdout tail ---\n%s", code,
                  tail.c_str());
        } else {
            ++g_checks;
        }
        // ★ 恰好一行：多了 = 被无条件打 / 打成了周期行；零行 = arm 没了或打印被删。
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(countOf(stdout_text, "token     : ")), 1);
        const std::string line = tokenLine(stdout_text);
        LIFT_CHECK(!line.empty());
        // 形状 = `violations=<n> max-concurrent=<m>`；`first=` 只在违规非 0 时出现（与解释器同一条）。
        const long long violations = fieldValue(line, "violations=");
        const long long max_concurrent = fieldValue(line, "max-concurrent=");
        LIFT_CHECK(violations >= 0);
        LIFT_CHECK(max_concurrent >= 1);
        const bool has_first = contains(line, " first=");
        // ★ 这里**故意不要求 violations==0**：非 0 是另一个缺陷（见文件头），⛔ 不许在这里改弱 / 归零。
        LIFT_CHECK_EQ_I64(has_first ? 1 : 0, violations == 0 ? 0 : 1);
        if (has_first) {
            // `first=` 后面必须点名一个站点（⛔ 不许只打一个空的 `first=`）。
            const std::size_t at = line.find(" first=");
            LIFT_CHECK(at + 7 < line.size());
        }
    }

    // ---- ② 缺省档：那一行 + Phase-0 四行都不许出现（缺省档 stdout 逐字不变）----
    g_current_case = "aot-token-summary/default-gated";
    {
        const fs::path appdata = root / "appdata-default";
        fs::create_directories(appdata, ec);
        LIFT_CHECK(!ec);
        const std::vector<std::string> args = {"--present", "headless", "--audio", "off",
                                               "--exe", exec_pe.string(), "--max-frames", "1",
                                               "--game-dir", game_dir.string(), "--appdata", appdata.string(),
                                               "--time-limit", "120"};
        const fs::path out = root / "default.out";
        const fs::path err = root / "default.err";
        const int code = runChild(aot_exe, args, root, out, err);
        const std::string stdout_text = readAll(out);
        if (code != 0) {
            const std::string tail = stdout_text.size() > 4000 ? stdout_text.substr(stdout_text.size() - 4000) : stdout_text;
            failf(__FILE__, __LINE__, "缺省档的短跑应 exit 0，实际 exit=%d\n--- stdout tail ---\n%s", code,
                  tail.c_str());
        } else {
            ++g_checks;
        }
        // 先证明这一跑真的跑完并打了摘要（否则"没有 token 行"可能只是"什么都没跑"）。
        LIFT_CHECK(contains(stdout_text, "frames    : 1"));
        // 闸门：缺省档不许有这条读数，也不许有紧邻的 Phase-0 四行。
        LIFT_CHECK(!contains(stdout_text, "token     : "));
        LIFT_CHECK(!contains(stdout_text, "tokenhold : "));
        LIFT_CHECK(!contains(stdout_text, "holdhist  : "));
        LIFT_CHECK(!contains(stdout_text, "waithist  : "));
        LIFT_CHECK(!contains(stdout_text, "shimmax   : "));
    }

    fs::remove_all(root, ec);  // 诊断文件不留在 temp 里；失败时也删（stdout 已经把内容打出去了）
    return finish("aot-token-summary");
}
