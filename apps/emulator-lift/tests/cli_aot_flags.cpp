// emulator-lift · **AOT 臂**的 `--cursor-warp` / `--sound` 参数面（CLI 级 + 真子进程）
//
// 判据（红了意味着什么）：
//   ① `--help` 里 `--cursor-warp` 与 `--sound` **都在**。
//      红了 ⇒ 用户按 README / 解释器臂的写法给参数时，AOT 臂连"有这个东西"都不知道。
//   ② `--cursor-warp --present headless` ⇒ exit 2，stderr 是**"headless 不接受窗口参数"**那句
//      （含 `--present headless` 与 `--cursor-warp`），**不含** `未知参数`。
//      红了 ⇒ 一个必然无效的开关被当成拼错的参数打回（或反过来被静默接受）。
//   ③ 错拼 `--cursor-wrap`（wrap ≠ warp）**仍然**是 `未知参数` + exit 2。
//      红了 ⇒ 参数解析退化成"接受任何参数"，② 的绿就没有意义（§12.5 的 `--native-overrides`
//      那条纪律：⛔ 不许靠"什么都收"让守卫变绿）。
//   ④ `--sound` ⇒ **被接受**（不是 `未知参数`）并打**一条**一次性说明：含理由（本臂没有音频管线）
//      与跟踪单占位 `TODO(REQ)`；且这条说明在这份 stderr 里**恰好出现一次**。
//      红了 ⇒ 要么"指定了就跑不起来"（另一种"无法指定"），要么静默接受（用户以为该有声）。
//   ⑤ 缺省档（**不给** `--sound`）这条说明**不出现**（同一份 stderr 面的对照组）。
//
// ★ 四条主断言**全部判在 GPU bring-up 之前**：AOT 臂的参数校验（`aot_main.cpp` 的
//   `--audio` / `--cursor-warp` 平台闸门 / `present_window_flag` / `--sound` 说明）都在
//   `SdlWindow::init`（窗口 + GPU device）**之前** ⇒ 本用例**不需要 GPU、不需要任何资产**
//   （不读 LFS、不碰游戏安装、不给 `--exe`：走到执行源定位时才失败，而那时判据已经判完）。
//   ⇒ 这里**没有** `LIFT_SKIP_IF_NO_GPU`：判据不依赖它，加了反而会让"没 GPU 就整条不判"。
// ★ 为什么是 CLI 级 + 真子进程：判据穿过"参数解析 → 早期拒绝 → 文案 → 退出码"整条链；
//   单测某个解析函数抓不到"解析对了但闸门漏了 / 文案还写着未知参数"这种半修。
// ★ 子进程 cwd 指到临时目录（照 `tests/cli_cursor_warp.cpp:17-18` 的理由）⇒ 结果不随宿主 cwd 漂。
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "check.h"
#include "cmdline.h"

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

#if defined(_WIN32)

namespace fs = std::filesystem;

struct ChildRun {
    DWORD exit_code = 0xFFFFFFFFu;
    bool started = false;
    bool timed_out = false;
    std::string out;
    std::string err;
};

std::string readAllBytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

using lift_test::quoteArg;

ChildRun runChild(const fs::path& exe, const std::wstring& args, const fs::path& cwd, const fs::path& out_path,
                  const fs::path& err_path) {
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
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = h_out;
    si.hStdError = h_err;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = quoteArg(exe.wstring()) + L" " + args;
    const BOOL ok = ::CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                     cwd.c_str(), &si, &pi);
    ::CloseHandle(h_out);
    ::CloseHandle(h_err);
    if (!ok) return r;
    r.started = true;
    // 30 s 上限：这几条都在建窗口之前就返回；卡住必须**红**，⛔ 不许挂死流水线。
    const DWORD waited = ::WaitForSingleObject(pi.hProcess, 30000u);
    if (waited != WAIT_OBJECT_0) {
        r.timed_out = true;
        ::TerminateProcess(pi.hProcess, 991u);
        ::WaitForSingleObject(pi.hProcess, 5000u);
    }
    ::GetExitCodeProcess(pi.hProcess, &r.exit_code);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    r.out = readAllBytes(out_path);
    r.err = readAllBytes(err_path);
    return r;
}

bool hasLineStartingWith(const std::string& text, const std::string& prefix) {
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t nl = text.find('\n', pos);
        const std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (line.compare(0, prefix.size(), prefix) == 0) return true;
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return false;
}

/** `needle` 在 `hay` 里出现几次（④ 的"**恰好一次**"判据）。 */
std::size_t countOccurrences(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return 0;
    std::size_t n = 0;
    std::size_t pos = hay.find(needle);
    while (pos != std::string::npos) {
        ++n;
        pos = hay.find(needle, pos + needle.size());
    }
    return n;
}

// ★ ④ 用来认"那条一次性说明"的**唯一**标记：说明的开头。⛔ 不用泛词（`--sound` 会命中 help /
//   其它文案）—— 标记必须只属于这句话，否则"恰好一次"这条断言会变成恒真。
const char* const kSoundNoticeMarker = "--sound 已接受";

#endif  // _WIN32

}  // namespace

int main(int argc, char** argv) {
    using namespace lift_test;

#if !defined(_WIN32)
    (void)argc;
    (void)argv;
    LIFT_SKIP("AOT 臂的参数面用 CreateProcessW 起子进程（Windows 专有）");
#else
    if (argc < 2) {
        LIFT_SKIP("需要 CTest 传 <emulator-lift-aot.exe>（见 CMakeLists 的 cli-aot-flags）");
    }
    const fs::path exe = fs::path(argv[1]);
    LIFT_CHECK(fs::exists(exe));

    std::error_code ec;
    const fs::path root = fs::temp_directory_path(ec) / ("lift-cli-aot-" + std::to_string(::GetCurrentProcessId()));
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    LIFT_CHECK(!ec);

    // ---- ① `--help`：两个 flag 都在（缺一个都红）----
    g_current_case = "help-lists-both-flags";
    {
        const ChildRun r = runChild(exe, L"--help", root, root / "help.out", root / "help.err");
        LIFT_CHECK(r.started);
        LIFT_CHECK(!r.timed_out);
        LIFT_CHECK_EQ_U32(r.exit_code, 0u);
        LIFT_CHECK(r.out.find("--cursor-warp") != std::string::npos);
        LIFT_CHECK(r.out.find("--sound") != std::string::npos);
        // ★ S4：`--input-plan` 必须在 help 里（否则用户不知道 AOT 臂有输入控制面）。
        LIFT_CHECK(r.out.find("--input-plan") != std::string::npos);
    }

    // ---- ①' `--input-plan <不存在的文件>` ⇒ exit 2 且 stderr **点名那个路径** ----
    //      红了 ⇒ 计划文件被静默忽略（那正是"以为按了、其实没按"的失效方式）。
    g_current_case = "input-plan-missing-file";
    {
        const fs::path missing = root / "no-such-plan.plan";
        const ChildRun r = runChild(exe, L"--input-plan " + quoteArg(missing.wstring()), root,
                                    root / "planmiss.out", root / "planmiss.err");
        LIFT_CHECK(r.started);
        LIFT_CHECK(!r.timed_out);
        LIFT_CHECK_EQ_U32(r.exit_code, 2u);
        LIFT_CHECK(r.err.find("未知参数") == std::string::npos);
        LIFT_CHECK(r.err.find("no-such-plan.plan") != std::string::npos);
    }

    // ---- ①'' `--input-plan <语法错文件>` ⇒ exit 2 且**点出行号**（`<path>:3:`）----
    //      红了 ⇒ 语法错被当成"空计划"吞掉，或者报错不带位置（人无法修）。
    g_current_case = "input-plan-syntax-error-names-line";
    {
        const fs::path bad = root / "bad.plan";
        {
            std::ofstream out(bad, std::ios::binary);
            out << "# 语法错：presents 后面不是数字\n";
            out << "await always\n";
            out << "click 1 2\n";
            out << "await presents abc\n";
        }
        const ChildRun r = runChild(exe, L"--input-plan " + quoteArg(bad.wstring()), root, root / "planbad.out",
                                    root / "planbad.err");
        LIFT_CHECK(r.started);
        LIFT_CHECK(!r.timed_out);
        LIFT_CHECK_EQ_U32(r.exit_code, 2u);
        LIFT_CHECK(r.err.find("未知参数") == std::string::npos);
        // 行号 = 文件里第 4 行（`await presents abc`）—— 判据是"报错点出**那一行**"，不是"有行号"。
        LIFT_CHECK(r.err.find("bad.plan:4:") != std::string::npos);
        LIFT_CHECK(r.err.find("abc") != std::string::npos);
    }

    // ---- ② `--cursor-warp` + 缺省呈现档（headless）⇒ exit 2，且是"headless 不接受窗口参数"----
    //      ⛔ 不是 `未知参数`：那正是改动前的形状（用户"无法指定"的现场）。
    g_current_case = "headless-rejects-cursor-warp";
    {
        const ChildRun r = runChild(exe, L"--cursor-warp --present headless", root, root / "cw.out",
                                    root / "cw.err");
        LIFT_CHECK(r.started);
        LIFT_CHECK(!r.timed_out);
        LIFT_CHECK_EQ_U32(r.exit_code, 2u);
        LIFT_CHECK(r.err.find("未知参数") == std::string::npos);
        LIFT_CHECK(r.err.find("--present headless") != std::string::npos);
        LIFT_CHECK(r.err.find("--cursor-warp") != std::string::npos);
        // 启动前拒绝 ⇒ **一行运行摘要都没有**（不是"跑了一部分才停"）
        LIFT_CHECK(!hasLineStartingWith(r.out, "stop"));
        LIFT_CHECK(!hasLineStartingWith(r.out, "insns"));
    }

    // ---- ③ 错拼（wrap ≠ warp）**仍然**是未知参数：钉住"② 不是靠接受任何参数"----
    g_current_case = "typo-cursor-wrap-still-unknown";
    {
        const ChildRun r = runChild(exe, L"--cursor-wrap --present headless", root, root / "typo.out",
                                    root / "typo.err");
        LIFT_CHECK(r.started);
        LIFT_CHECK(!r.timed_out);
        LIFT_CHECK_EQ_U32(r.exit_code, 2u);
        LIFT_CHECK(r.err.find("未知参数") != std::string::npos);
        LIFT_CHECK(r.err.find("--cursor-wrap") != std::string::npos);
    }

    // ---- ④ `--sound`：**接受** + 一条一次性说明（含理由 + 跟踪单占位），⛔ 不是 `未知参数` ----
    //      ★ 不判退出码：`--sound` 之后本臂仍会因"没有执行源 / 没有 GPU"而 exit 2 —— 那与
    //        `--sound` 无关，判据是"它被接受 + 说了实话"，在 GPU bring-up 之前就已成立。
    g_current_case = "sound-accepted-with-once-notice";
    {
        const ChildRun r = runChild(exe, L"--sound", root, root / "sound.out", root / "sound.err");
        LIFT_CHECK(r.started);
        LIFT_CHECK(!r.timed_out);
        LIFT_CHECK(r.err.find("未知参数") == std::string::npos);
        LIFT_CHECK(countOccurrences(r.err, kSoundNoticeMarker) == 1u);
        // ② 理由必须在：本臂没有音频管线（不是"暂不支持"这种含糊话）。
        LIFT_CHECK(r.err.find("没有音频管线") != std::string::npos);
        // ③ 跟踪单 id 必须**是真的**（`REQ-` + 26 位 ULID），⛔ 不许把占位符发出去。
        //    ★ 按**形态**判、不锚具体 id：换单号不该让守卫红，但留着 `TODO(` 必须红。
        LIFT_CHECK(r.err.find("REQ-") != std::string::npos);
        LIFT_CHECK(r.err.find("TODO(") == std::string::npos);
    }

    // ---- ⑤ 对照组：**不给** `--sound` ⇒ 那条说明不出现（同一份 stderr 面）----
    g_current_case = "default-no-sound-notice";
    {
        const ChildRun r = runChild(exe, L"--present headless --present-hidden", root, root / "nosound.out",
                                    root / "nosound.err");
        LIFT_CHECK(r.started);
        LIFT_CHECK(!r.timed_out);
        LIFT_CHECK_EQ_U32(r.exit_code, 2u);
        LIFT_CHECK(r.err.find("--present headless") != std::string::npos);
        LIFT_CHECK(countOccurrences(r.err, kSoundNoticeMarker) == 0u);
        // ★ S4 的对照：**不给** `--input-plan` ⇒ 那一行摘要**不出现**（缺省档逐字节不变的机械判据）。
        LIFT_CHECK(r.out.find("input-plan:") == std::string::npos);
        LIFT_CHECK(r.err.find("input-plan:") == std::string::npos);
    }

    fs::remove_all(root, ec);
#endif

    return lift_test::finish("cli-aot-flags");
}
