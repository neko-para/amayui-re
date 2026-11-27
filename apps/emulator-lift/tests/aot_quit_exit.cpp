// emulator-lift · **TITLE→Quit 退出路径**的端到端**复现件**（`LIFT_STRESS` 档；真 run）
//
// ★ 档位：这条**真跑原版安装树**（`links/raw` + TITLE 界面），判据是"guest 自己的退出
//   路径走完"（`Command_Exit_Exception` 的 catch 续延 → `ExitProcess`），⛔ 不是某个 shim 接口的
//   合同 ⇒ 按用途只能进 `if(DEFINED ENV{LIFT_STRESS})`（唯一允许跑真游戏的一档）。
//   ⇒ 默认档的**机制守卫**是 `lift-closure-imm32`（顶层无条件注册：候选集 + 派发表里必须有三行
//     `LIFT_AOT_ENTRY`，不跑游戏）。本文件是它的**端到端复现件**，不是门禁。
//
// ★ 驱动器（2026-11-27 用户裁决：`--input-plan` / `.plan` 整体退役）：JS 插件
//   `scripts/plugins/quit-at-title.js`（`bootToTitle` 门 + TITLE 上一条 `ctx.input.click(1180,640)`）
//   经 `--plugin <绝对路径>`（CTest 第 5 个参数）装载。⛔ 插件**不许**用 `ctx.quit()` /
//   `ctx.finish()` 代替 guest 退出 —— 那测的就不是 guest 自己的 Quit 路径了。
//
// 判据（红了意味着什么）：
//   ① **正常退出**：TITLE 门命中后，click 被 guest 消费，AOT 臂走完 guest 自己的退出路径
//      （`Command_Exit_Exception` 的 catch 续延 → `ExitProcess`）⇒ 宿主进程 **exit 0**，
//      摘要为 `stop      : stopped`，派发表 `misses=0`。
//      ⛔ **不含** `达到 --time-limit`：那条是"用时间上限把 exit 0 蒙出来"的圆场（时间上限也
//      exit 0）⇒ 它出现就说明 guest 的 Quit 根本没被走到，本用例必须红。
//   ② **没有响亮错**：stdout / stderr 里都**没有** `emulator-lift-aot: ⛔` · `派发表里没有的 EA` ·
//      `decode-error`。红了意味着退出路径又停在派发表外（典型：D26 的未对齐 imm32 扫描退化成对齐扫描）。
//   ③ **反向对照：真失败仍然响亮**：同一个可执行体 + `--exe <links/raw/AGE.EXE>`
//      （sha256 与构建期 pin 不一致）⇒ **exit 2** 且 stderr 有 `pin 核对失败` / `不一致`。
//      ⇒ 红了意味着有人把"退出路径正常收口"扩大成了"什么都 exit 0"。
//
// ★ 为什么是**真 run**（不是纯夹具）：退出路径是 guest 字节 + SEH 机器 + 派发表三者共同的产物；
//   纯夹具只能钉其中一段。本用例的可执行体就是构建期 pin 住的那份 lifted 代码。
// ★ 代价：一次 run 几秒（mac arm64 / RelWithDebInfo / headless；`--time-limit 120` 只是兜底）。
//   CTest 用 420 s 上限（win32 未实测，留足余量）。
// ★ 输入（执行源 / `links/raw` / 插件入口）假定在场；缺了 `LIFT_SKIP`（77）。
//   ⛔ 不许"什么都不做然后 return 0"（那与跑绿了长得一样，见 `check.h`）。
// ★ 子进程输出**重定向到文件**（⛔ 不走管道：受限沙箱里管道会 EPERM；与 `tools/corpus.mjs`
//   的 `runCapture` 同一口径）。
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

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// ★ **产品响亮错的规范前缀**（`src/aot/aot_main.cpp:1489`：`emulator-lift-aot: ⛔ 停在一个派发表里
//   没有的 EA：…`）。本用例判"有没有响亮错"一律用这个前缀，⛔ 不用裸 `⛔`（见 ①+② 那段的注释）：
//   `⛔` 在正常读数的解释性文案里也有（`lift_dyn` 行 / `[warn] [present]` 行），裸判 = 假红。
constexpr const char* kLoudErrorPrefix = "emulator-lift-aot: ⛔";

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
int runChild(const fs::path& exe, const std::vector<std::string>& args, const fs::path& cwd,
             const fs::path& out_path, const fs::path& err_path) {
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
    // ★ 300 s 上限：正例本机 60–90 s；卡住必须**红**（CTest 侧还有 TIMEOUT 兜底）。
    const DWORD waited = ::WaitForSingleObject(pi.hProcess, 300000u);
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
    // ★ 子进程 cwd 指到临时目录：产物（日志 / 帧 dump）不往仓库里写。
    const std::string wrapped = "cd " + shq(cwd.string()) + " && " + cmd;
    const int status = std::system(wrapped.c_str());
    if (status == -1) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
#endif
}

}  // namespace

int main(int argc, char** argv) {
    using namespace lift_test;

    if (argc < 6) {
        LIFT_SKIP("需要 CTest 传 <emulator-lift-aot> <links/raw> <links/raw/AGE.EXE> "
                  "<corpus/assets/age/AGE.EXE__dumped.sectfix.EXE> <插件入口 quit-at-title.js>（见 CMakeLists）");
    }
    const fs::path aot_exe = argv[1];
    const fs::path game_dir = argv[2];
    const fs::path wrong_pe = argv[3];
    const fs::path right_pe = argv[4];
    const fs::path plugin = argv[5];

    std::error_code ec;
    if (!fs::exists(aot_exe, ec)) LIFT_SKIP("emulator-lift-aot 不在场");
    if (!fs::is_directory(game_dir, ec)) LIFT_SKIP("links/raw 不在场（原版安装树）");
    if (!fs::exists(wrong_pe, ec)) LIFT_SKIP("links/raw/AGE.EXE 不在场（反向对照需要一份 sha 不同的 PE）");
    if (!fs::exists(right_pe, ec)) LIFT_SKIP("执行源（corpus/assets/age/AGE.EXE__dumped.sectfix.EXE）不在场");
    if (!fs::exists(plugin, ec)) LIFT_SKIP("插件入口 scripts/plugins/quit-at-title.js 不在场");

    const fs::path root =
        fs::temp_directory_path(ec) / ("lift-aot-quit-exit-" + std::to_string(
#if defined(_WIN32)
                                                           ::GetCurrentProcessId()
#else
                                                           ::getpid()
#endif
                                                           ));
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    LIFT_CHECK(!ec);
    const fs::path appdata = root / "appdata";
    fs::create_directories(appdata, ec);
    LIFT_CHECK(!ec);

    const std::vector<std::string> common = {
        "--present", "headless", "--skip-logo", "--audio", "off", "--game-dir",
        game_dir.string(), "--appdata", appdata.string(), "--time-limit", "120",
        // ★ 判据行（`misses=0` 在收口 readout 里）在默认档**不打**（2026-11-27 用户判据：
        //   默认档只留出错 / 警告 + 插件日志 + 1 行 `stop`）⇒ 本用例显式开 `--log-level info`
        //   才能读到它。⛔ 判据本身没放宽：`stop` / `misses=0` / 不含 time-limit 仍按原文判。
        "--log-level", "info"};

    // ---- ① + ② 正例：TITLE 门 → click Quit ⇒ guest 退出 ⇒ exit 0，且没有响亮错 ----
    g_current_case = "aot-quit/exit-zero";
    {
        std::vector<std::string> args = common;
        // ★ 子进程 cwd 是临时目录 ⇒ `--exe` / `--plugin` 必须是**绝对路径**（AOT 不会从 cwd
        //   找仓库内入库件；插件入口也一样 —— CTest 给的是 `${CMAKE_CURRENT_SOURCE_DIR}/...`）。
        args.push_back("--exe");
        args.push_back(right_pe.string());
        args.push_back("--plugin");
        args.push_back(plugin.string());
        const fs::path out = root / "quit.out";
        const fs::path err = root / "quit.err";
        const int code = runChild(aot_exe, args, root, out, err);
        const std::string stdout_text = readAll(out);
        const std::string stderr_text = readAll(err);
        if (code != 0) {
            failf(__FILE__, __LINE__,
                  "TITLE→Quit 应正常退出（exit 0），实际 exit=%d\n--- stdout tail ---\n%s\n"
                  "--- stderr tail ---\n%s",
                  code, stdout_text.substr(stdout_text.size() > 4000 ? stdout_text.size() - 4000 : 0).c_str(),
                  stderr_text.substr(stderr_text.size() > 4000 ? stderr_text.size() - 4000 : 0).c_str());
        } else {
            ++g_checks;
        }
        // ★ 判据是**产品响亮错的规范前缀** `emulator-lift-aot: ⛔`（`src/aot/aot_main.cpp:1489` 是
        //   本臂唯一一处响亮错：`emulator-lift-aot: ⛔ 停在一个派发表里没有的 EA：…`），⛔ 不是裸 `⛔`。
        //   为什么收窄（2026-11-24 父级裁决）：`⛔` 在**解释性文案**里也出现 —— 正常读数
        //   `lift_dyn : … （⛔ 不进闭式…）` 与 `[warn] [present] 最后帧 hash 不可用 … （⛔ 不是旧值…）`
        //   ⇒ 裸判会把"跑绿了"判成红（本用例在 `f58c2b7` 上就是这么红的：exit 0 / 173 帧 / 正常结局）。
        //   ⛔ 这不是放宽：真响亮错一定会带那个前缀，而它下面三条（`派发表里没有的 EA` / `decode-error`）
        //   仍然按**原文**判，两条口径互不替代。
        LIFT_CHECK(!contains(stdout_text, kLoudErrorPrefix));
        LIFT_CHECK(!contains(stderr_text, kLoudErrorPrefix));
        LIFT_CHECK(!contains(stdout_text, "派发表里没有的 EA"));
        LIFT_CHECK(!contains(stderr_text, "派发表里没有的 EA"));
        LIFT_CHECK(!contains(stdout_text, "decode-error"));
        LIFT_CHECK(!contains(stderr_text, "decode-error"));
        // 正常结局 + 派发表零 miss（退出路径上的每个 EA 都在表里）。
        LIFT_CHECK(contains(stdout_text, "stop      : stopped"));
        LIFT_CHECK(contains(stdout_text, "misses=0"));
        // ★ 防"用时间上限蒙出 exit 0"：停因必须是 guest 自己收的口，⛔ 不是 `--time-limit`。
        LIFT_CHECK(!contains(stdout_text, "达到 --time-limit"));
        LIFT_CHECK(!contains(stderr_text, "达到 --time-limit"));
    }

    // ---- ③ 反向对照：错误执行源仍然**响亮**失败（exit 2），不是 0 ----
    g_current_case = "aot-quit/wrong-source-loud";
    {
        std::vector<std::string> args = common;
        args.push_back("--exe");
        args.push_back(wrong_pe.string());
        const fs::path out = root / "pin.out";
        const fs::path err = root / "pin.err";
        const int code = runChild(aot_exe, args, root, out, err);
        const std::string stdout_text = readAll(out);
        const std::string stderr_text = readAll(err);
        LIFT_CHECK_EQ_I64(code, 2);
        LIFT_CHECK(contains(stderr_text, "pin 核对失败"));
        LIFT_CHECK(contains(stderr_text, "不一致"));
        // ⛔ 反向对照不能"跑起来然后正常停"：它必须在 run 之前就拒绝。
        LIFT_CHECK(!contains(stdout_text, "stop      :"));
    }

    fs::remove_all(root, ec);
    return finish("aot-quit-exit");  // 与 CTest 注册名一致（cmake-test-entry 守卫的判据）
}
