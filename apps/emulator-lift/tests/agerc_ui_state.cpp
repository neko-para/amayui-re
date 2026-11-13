// emulator-lift · AGERC `_ShowDialog@12(-1)` = **UI 状态查询**（不是对话框）的守卫（assets 档）
//
// 判据（红了意味着什么）：
//   ① 进 TITLE.BIN 后把光标移到客户区顶边（y=1）⇒ 引擎的 100 ms 悬停计时器
//      `sub_4B9240` 会调 `_ShowDialog(-1, hwnd, 0)`（EA 0x4B929F），返回后停在 0x4B92A5。
//   ② trace 里**恰好**是 `agerc!_ShowDialog@12(FFFFFFFF,00010000,00000000) -> 00000001`：
//      本线**故意**回答非 0（= “已有 AGERC UI 开着”），把引擎挡在它自己的 AGERC 菜单栏路径
//      （`_ShowDialog(4)` ⇒ `sub_408620` ⇒ `GetWindowRect(返回值)`）之外。
//      真机此处是 0（无框），但本线不实现 AGERC 菜单栏 / 设置对话框 ⇒ 回答 0 会让引擎立刻
//      拿我们 stub 的 `id==4` 返回值去 `GetWindowRect`（不存在的句柄 ⇒ loud failure / abort）。
//   ③ 不许出现 `id=4294967295` 的 **warn**（它是状态查询，不是“不支持的对话框”）、
//      不许出现 `id=4` 的菜单栏尝试、不许 `窗口句柄 0x00000001 不存在`。
//   ④ run 的收口必须是 `until-ea`（不是 abort / 窗口关闭）。
//
// ★ 为什么是 assets 档：要真 boot 到 TITLE.BIN（约 15 s）+ 真窗口管理器（`--present window
//   --present-hidden`；SDL dummy 下窗口对象仍存在，WM_MOUSEMOVE 走同一条 `runtime/input`）。
//   缺安装树 / 执行源 / 窗口 ⇒ 自己 SKIP 77。
// ★ 为什么 `--until ea 0x4B92A5`：就在 shim 返回之后；比“跑满 15 s 再看有没有 spamming”更确定，
//   而且这条 EA 只在 `_ShowDialog(-1)` 返回点上出现 ⇒ 时序不敏感。
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "check.h"
#include "subprocess_skip.h"
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

constexpr const char* kUntilEa = "0x4B92A5";

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

// ★ GPU 拒绝启动**不在这里**：它由 `LIFT_SKIP_IF_NO_GPU` 单独判（要求本机实探无 GPU）。
bool environmentUnavailable(const std::string& log) {
    return contains(log, "找不到执行源") || contains(log, "SDL_Init(VIDEO)") || contains(log, "--game-dir") ||
           contains(log, "--present window 失败");
}

}  // namespace

int main(int argc, char** argv) {
    using namespace lift_test;
    const std::vector<std::string> args = lift::argvUtf8(argc, argv);
    if (args.size() < 4) {
        std::fprintf(stderr, "用法：lift-agerc-ui-state <emulator-lift> <game-dir> <work-dir>\n");
        LIFT_CHECK(false);
        return finish("agerc-ui-state");
    }
    const fs::path exe = args[1];
    const std::string game_dir = args[2];
    const fs::path work = args[3];
    std::error_code ec;
    fs::remove_all(work, ec);
    fs::create_directories(work, ec);

    // 触发：进 TITLE.BIN 之后把光标放到客户区顶边（y=1 < 2）⇒ 引擎的悬停计时器开始跑。
    const fs::path script = work / "hover.txt";
    {
        std::ofstream out(script, std::ios::binary);
        out << "at marker TITLE.BIN\nmouse_move 640 1\n";
    }

    const fs::path appdata = work / "appdata";
    const fs::path log_path = work / "run.log";
    const std::vector<std::string> child_args = {
        "--present", "window",
        "--present-hidden",
        "--present-driver", "software",
        "--skip-logo",
        "--audio", "off",
        "--game-dir", game_dir,
        "--appdata", appdata.string(),
        "--until", "ea", kUntilEa,
        "--trace-shims",
        "--input-script", script.string(),
        "--time-limit", "600",
    };
    const ChildResult child = runChild(exe, child_args, log_path);
    const std::string log = readAllBytes(log_path);
    if (!child.started) LIFT_SKIP("起不了子进程（emulator-lift 路径不对？）");
    if (child.exit_code != 0) {
        // ★ 无 GPU：子进程在跑 guest 之前就拒绝启动 ⇒ 环境缺失，SKIP 77（原因进输出）。
        LIFT_SKIP_IF_NO_GPU(child.exit_code, log);
        if (environmentUnavailable(log)) LIFT_SKIP("环境不可用（没有执行源 / 安装树 / 窗口）");
        std::fprintf(stderr, "exit %d；log 前 4000 字节：\n%.4000s\n", child.exit_code, log.c_str());
        LIFT_CHECK(false);
        return finish("agerc-ui-state");
    }

    // ① 输入真的在 TITLE.BIN 后注入、悬停计时器真的调到了 `_ShowDialog(-1)` 的返回点。
    LIFT_CHECK(contains(log, "name=TITLE.BIN ⇒ mouse_move"));
    LIFT_CHECK(contains(log, "stop      : until-ea"));
    LIFT_CHECK(contains(log, "eip=004B92A5"));

    // ② 返回值 = 本线**故意**的 1（= “已有 AGERC UI”，挡住未实现的菜单栏路径）。
    LIFT_CHECK(contains(log, "agerc!_ShowDialog@12(FFFFFFFF,00010000,00000000) -> 00000001"));

    // ③ 没有把状态查询当成“不支持的对话框”warn、没有去开菜单栏、没有对不存在的句柄 GetWindowRect。
    LIFT_CHECK(!contains(log, "id=4294967295"));
    LIFT_CHECK(!contains(log, "agerc!_ShowDialog@12(00000004,00010000,00000000)"));
    LIFT_CHECK(!contains(log, "窗口句柄 0x00000001 不存在"));

    std::printf("agerc-ui-state: _ShowDialog(-1) -> 1（UI 状态查询；menu-bar 路径已挡住）\n");
    return finish("agerc-ui-state");
}
