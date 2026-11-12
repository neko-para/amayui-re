// emulator-lift · AGERC 关窗确认三臂（assets 档）
//
// 判据（红了意味着什么）：
//   ① `--close-confirm exit|title|cancel` 三档**不弹系统框**、直接按选择返回 1/2/0，并各自走到
//      真机 close handler 的对应块起点：`0x405B21`（exit）/ `0x405A82`（title）/ `0x405A95`（cancel）。
//   ② `close_window`（宿主动作）真的把窗口 X 送进引擎：日志里有 `TITLE.BIN ⇒ close_window` 的
//      marker 边沿，且 trace 里 `agerc!_ShowDialog@12(00000003,00010000,00000000) -> 0000000X`。
//   ③ title/cancel 的返回值 2/0 必须让 run **继续**（stop 是 `until-ea`，不是 `stopped`/窗口关闭）——
//      这就是"取消那次关窗请求"的机械判据（宽限 deadline 在系统框/策略评估时早已过期）。
//
// ★ 为什么全用 policy 臂而不测 `ask`：`ask` 会弹**系统对话框**等人（CTest 里没有可点的桌面），
//   本机用 dummy 视频时行为由 SDL 决定、不适合做 gate。`ask` 的"系统框给什么就返回什么 / 起不来
//   回默认值 / 2/0 取消关窗"由 `tests/messagebox.cpp` 的假 sink 逐条断言。
//
// ★ 注册在 assets 档（判据 = 注册位置：`if(EXISTS ${LIFT_ASSETS_GAME_DIR}/SYS4INI.BIN)`）；
//   需要真 boot 到 TITLE.BIN（约 14 s/臂）。缺安装树 / 执行源 ⇒ 自己 SKIP 77。
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "check.h"
#include "runtime/host_encoding.h"

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

// 真机三臂的块起点（`sub_4057B0` 的返回路径；EA 锚见需求单
// `REQ-01M72JFZ87DJNT52DW4HB8SEV6` §1/§4）。
constexpr const char* kExitEa = "0x405B21";
constexpr const char* kTitleEa = "0x405A82";
constexpr const char* kCancelEa = "0x405A95";

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

bool environmentUnavailable(const std::string& log) {
    return contains(log, "找不到执行源") || contains(log, "拒绝启动") || contains(log, "SDL_Init(VIDEO)") ||
           contains(log, "--game-dir") || contains(log, "--present window 失败");
}

struct Arm {
    const char* policy;
    const char* until_ea;
    std::uint32_t expected_result;
};

}  // namespace

int main(int argc, char** argv) {
    using namespace lift_test;
    const std::vector<std::string> args = lift::argvUtf8(argc, argv);
    if (args.size() < 4) {
        std::fprintf(stderr, "用法：lift-close-confirm <emulator-lift> <game-dir> <work-dir>\n");
        LIFT_CHECK(false);
        return finish("close-confirm");
    }
    const fs::path exe = args[1];
    const std::string game_dir = args[2];
    const fs::path work = args[3];
    std::error_code ec;
    fs::remove_all(work, ec);
    fs::create_directories(work, ec);
    // 触发：进入 TITLE.BIN 之后像用户点 X 一样请求关窗（`--input-script close_window` 宿主动作）。
    const fs::path script = work / "close.txt";
    {
        std::ofstream out(script, std::ios::binary);
        out << "at marker TITLE.BIN\nclose_window\n";
    }

    const Arm arms[] = {
        {"exit", kExitEa, 1},
        {"title", kTitleEa, 2},
        {"cancel", kCancelEa, 0},
    };
    for (const Arm& arm : arms) {
        g_current_case = arm.policy;
        const fs::path appdata = work / (std::string("appdata-") + arm.policy);
        const fs::path log_path = work / (std::string(arm.policy) + ".log");
        const std::vector<std::string> child_args = {
            "--present", "window",
            "--present-hidden",
            "--present-driver", "software",
            "--raster", "cpu",
            "--skip-logo",
            "--audio", "off",
            "--game-dir", game_dir,
            "--appdata", appdata.string(),
            "--close-confirm", arm.policy,
            "--until", "ea", arm.until_ea,
            "--trace-shims",
            "--input-script", script.string(),
            "--time-limit", "600",
        };
        const ChildResult child = runChild(exe, child_args, log_path);
        const std::string log = readAllBytes(log_path);
        if (!child.started) {
            LIFT_SKIP("起不了子进程（emulator-lift 路径不对？）");
        }
        if (child.exit_code != 0) {
            if (environmentUnavailable(log)) LIFT_SKIP("环境不可用（没有执行源 / 安装树 / 窗口）");
            std::fprintf(stderr, "%s 臂 exit %d；log 前 4000 字节：\n%.4000s\n", arm.policy, child.exit_code,
                         log.c_str());
            LIFT_CHECK(false);
            continue;
        }
        // ① 走到了对应返回路径的块起点（不是被宿主宽限/其它停机理掉）。
        LIFT_CHECK(contains(log, "stop      : until-ea"));
        LIFT_CHECK(contains(log, arm.until_ea + 2));
        LIFT_CHECK(!contains(log, "present: 窗口关闭"));
        // ② 关窗注入真的进了引擎：TITLE.BIN 边沿 + `_ShowDialog@12(id=3)` 的返回值。
        LIFT_CHECK(contains(log, "marker edge: cur=1 id=0x00005264 name=TITLE.BIN ⇒ close_window"));
        char want_trace[160];
        std::snprintf(want_trace, sizeof(want_trace),
                      "agerc!_ShowDialog@12(00000003,00010000,00000000) -> %08X", arm.expected_result);
        LIFT_CHECK(contains(log, want_trace));
        // ③ 策略短路（不弹系统框）。
        LIFT_CHECK(contains(log, "不弹框、直接返回"));
        std::printf("%s arm: stop until-ea @ %s, _ShowDialog -> %u\n", arm.policy, arm.until_ea,
                    arm.expected_result);
    }
    return finish("close-confirm");
}
