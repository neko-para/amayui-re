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
//   ④ `--sound` ⇒ **被接受**（不是 `未知参数`）并打**一条**一次性说明：这条说明必须**与代码一致**
//      —— ⛔ 不许再说"本臂没有音频管线 / 不会有声音"（那是接线前的形状）。且这条说明在这份 stderr
//      里**恰好一次**。红了 ⇒ 要么"指定了就跑不起来"（另一种"无法指定"），要么静默接受。
//      ★ **洞 6**：这里⛔ **不**再断言"说明文本里含 `muted=0`"——那是**自证式断言**（只断言自己
//        打的那句话，`--sound` 的接线丢了它照绿）。真正的判据落在**引擎状态**上：
//        `aot-audio-wiring`（真 run，读收口摘要的 `audio     : … muted=0|1`，含 `muted=1` 的对照组）。
//      ★ **洞 4**：这段说明里"读数"那句必须限定"**仅** `--audio virtual|sdl` 或 `--audio-dump` 时
//        才有 `audio     :` 一行"（只给 `--sound` 时档位是 off ⇒ 没有那一行）。行为判据同样在
//        `aot-audio-wiring` 的①（那条 run 里那一行**不存在**）。
//   ⑤ 缺省档（**不给** `--sound`）这条说明**不出现**（同一份 stderr 面的对照组）。
//   ⑥ **S1 参数面**：`--audio virtual` / `--audio sdl` **不再**被拒（旧文案"本臂只有 off"必须消失）；
//      非法值 `--audio bogus` 仍 **exit 2** 并点名合法值域（⛔ 不放宽成"认不出就当 off"）；
//      `--audio-dump` 缺值 ⇒ exit 2 并点名这个 flag（⛔ 不静默当成"没有 dump"）。
//      红了 ⇒ 用户要么无法指定后端 / dump，要么非法值被静默吞掉。
//   ⑦ `--help` 里有 `--audio-dump`（否则用户不知道这条入口存在）；且它的说明**声明**了
//      「abort 之后 dump 不可信」（洞 7：`std::abort()` 路径跳过 `AudioEngine::end()` ⇒ WAV 两个
//      长度字段不回填。⛔ 不改 abort 语义，只声明）。
//   ⑧ `--profile-slow-frames <ms>`（按帧的 shim 分桶）：`--help` 里有它；**缺 `--profile` ⇒ exit 2
//      并点名两者**（含值 0 = 显式关那一档）；负数 / 小数 / 带单位 / 垃圾 / 缺值 ⇒ exit 2 且点名；
//      `--profile --profile-slow-frames 50` **被接受**。
//      红了 ⇒ 要么仪器"给了但跑不起来"，要么非法值被静默吞成 0（= 以为在量、其实没量）。
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
        // ★ S1：`--audio` 的三个档位与 `--audio-dump` 必须在 help 里（否则用户不知道它们存在）。
        LIFT_CHECK(r.out.find("--audio off|virtual|sdl") != std::string::npos);
        LIFT_CHECK(r.out.find("--audio-dump") != std::string::npos);
        // ★ 洞 7：`--audio-dump` 的说明必须**声明**「abort 之后 dump 不可信」（不改 abort 语义，只声明）。
        //   红了 ⇒ 那条已知的失效方式（`std::abort()` 跳过 `end()` ⇒ WAV 长度字段不回填）又变成
        //   "只有读过代码的人知道"。
        LIFT_CHECK(r.out.find("abort") != std::string::npos);
        LIFT_CHECK(r.out.find("不可信") != std::string::npos);
        // ★ S4：`--input-plan` 必须在 help 里（否则用户不知道 AOT 臂有输入控制面）。
        LIFT_CHECK(r.out.find("--input-plan") != std::string::npos);
        // ★ 诊断档 `--input-plan-trace` 也必须在 help 里（否则"配方 click 不被消费"只能靠猜；
        //   见 `REQ-01M7PG086DG4BBPA40VCVZTP4X` B）。⚠ 本用例在 POSIX 上 skip（CreateProcessW
        //   是 Windows 专有）⇒ mac 侧的等价判据在 `docs` / 手工命令，⛔ 不要因此删掉这一条。
        LIFT_CHECK(r.out.find("--input-plan-trace") != std::string::npos);
        // ★ S6：三条鼠标原语必须在 help 里（否则用户不知道"拖拽"能在配方里表达；
        //   词表与 `runtime/recipe.cpp` 的 `instructionList` 是**同一组**指令）。
        //   ⚠ 本用例在 POSIX 上 skip ⇒ 解析期判据另在 `input-plan-read-points` 的
        //   `input-plan/parse-errors` / `input-plan/mouse-primitives`（⛔ 不靠这一条当唯一判据）。
        LIFT_CHECK(r.out.find("mouse_move") != std::string::npos);
        LIFT_CHECK(r.out.find("mouse_down") != std::string::npos);
        LIFT_CHECK(r.out.find("mouse_up") != std::string::npos);
        // ★ `--profile-slow-frames` 必须在 help 里（否则"按帧的 shim 分桶"这条仪器等于不存在，
        //   而 wipe 剖面的最大缺口正是它 —— 见 `docs/03-emulator/wipe-profile.md`）。
        LIFT_CHECK(r.out.find("--profile-slow-frames") != std::string::npos);
        // ★ `--scale` 必须在 help 里，且说明里写明 `auto` 与 headless 的两条口径
        //   （否则用户不知道有这条入口，也不知道 headless 下它不生效）。
        LIFT_CHECK(r.out.find("--scale auto|数值") != std::string::npos);
        LIFT_CHECK(r.out.find("headless") != std::string::npos);
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

    // ---- ①'' `--input-plan <语法错文件>` ⇒ exit 2 且**点出行号**（`<path>:4:`）----
    //      红了 ⇒ 语法错被当成"空计划"吞掉，或者报错不带位置（人无法修）。
    //      ★ 坏行放在第 4 行：`expect …` 是**图语法**，plan 只认三条指令 ⇒ 未知指令。
    g_current_case = "input-plan-syntax-error-names-line";
    {
        const fs::path bad = root / "bad.plan";
        {
            std::ofstream out(bad, std::ios::binary);
            out << "# 语法错：第 4 行是图语法（--input-plan 不认）\n";
            out << "await script TITLE\n";
            out << "click 1 2\n";
            out << "expect screen changed\n";
        }
        const ChildRun r = runChild(exe, L"--input-plan " + quoteArg(bad.wstring()), root, root / "planbad.out",
                                    root / "planbad.err");
        LIFT_CHECK(r.started);
        LIFT_CHECK(!r.timed_out);
        LIFT_CHECK_EQ_U32(r.exit_code, 2u);
        LIFT_CHECK(r.err.find("未知参数") == std::string::npos);
        // 行号 = 文件里第 4 行 —— 判据是"报错点出**那一行**"，不是"有行号"。
        LIFT_CHECK(r.err.find("bad.plan:4:") != std::string::npos);
        LIFT_CHECK(r.err.find("未知指令") != std::string::npos);
        LIFT_CHECK(r.err.find("expect") != std::string::npos);
    }

    // ---- ①''' 旧指令一律**解析期硬错**：`await always` / `await presents <n>` / `key` ----
    //      红了 ⇒ 旧配方被静默接受（"按了但语义不对"），或报错不点名行 / 不点名"未知指令"。
    g_current_case = "input-plan-rejects-old-instructions";
    {
        struct OldInstr {
            const char* name;
            const char* text;
        };
        const OldInstr kOld[] = {
            {"always.plan", "await always\n"},
            {"presents.plan", "await presents 120\n"},
            {"key.plan", "key 13\n"},
        };
        for (const OldInstr& o : kOld) {
            const fs::path p = root / o.name;
            {
                std::ofstream out(p, std::ios::binary);
                out << o.text;
            }
            const std::string tag = std::string(o.name) + ".";
            const ChildRun r = runChild(exe, L"--input-plan " + quoteArg(p.wstring()), root, root / (tag + "out"),
                                        root / (tag + "err"));
            LIFT_CHECK(r.started);
            LIFT_CHECK(!r.timed_out);
            LIFT_CHECK_EQ_U32(r.exit_code, 2u);
            LIFT_CHECK(r.err.find("未知参数") == std::string::npos);
            LIFT_CHECK(r.err.find(std::string(o.name) + ":1:") != std::string::npos);
            LIFT_CHECK(r.err.find("未知指令") != std::string::npos);
        }
    }

    // ---- ①'''' 缺参数 / 负毫秒 / 单位后缀 / `=left` ⇒ exit 2 + `文件:行号:` ----
    g_current_case = "input-plan-rejects-bad-arguments";
    {
        struct BadArg {
            const char* name;
            const char* text;
            const char* word;
        };
        const BadArg kBads[] = {
            {"neg.plan", "wait -5\n", "正整数"},
            {"unit.plan", "wait 500 wall_ms\n", "wait"},
            {"missing.plan", "click 1\n", "缺参数"},
            {"eqleft.plan", "click 1 2 =left\n", "按钮词"},
            // 行内 `timeout` 的缺参 / 非数字 ⇒ 同样是 exit 2 + 行号（⛔ 不许静默当成 0）。
            {"to-missing.plan", "await script TITLE timeout\n", "毫秒数"},
            {"to-nan.plan", "await script TITLE timeout abc\n", "正整数"},
        };
        for (const BadArg& b : kBads) {
            const fs::path p = root / b.name;
            {
                std::ofstream out(p, std::ios::binary);
                out << b.text;
            }
            const std::string tag = std::string(b.name) + ".";
            const ChildRun r = runChild(exe, L"--input-plan " + quoteArg(p.wstring()), root, root / (tag + "out"),
                                        root / (tag + "err"));
            LIFT_CHECK(r.started);
            LIFT_CHECK(!r.timed_out);
            LIFT_CHECK_EQ_U32(r.exit_code, 2u);
            LIFT_CHECK(r.err.find("未知参数") == std::string::npos);
            LIFT_CHECK(r.err.find(std::string(b.name) + ":1:") != std::string::npos);
            LIFT_CHECK(r.err.find(b.word) != std::string::npos);
        }
    }

    // ---- ①''''' `timeout` **只能写在 `await …` 那一行的末尾**：跨行 / 挂在 `wait` / `click` 上 ⇒
    //      exit 2 + 点名行号 + 文案指向修法。
    //      红了 ⇒ 旧写法（"守上一条指令"）被静默接受，或者用户看不出该怎么改。
    g_current_case = "input-plan-rejects-stray-timeout";
    {
        struct StrayTimeout {
            const char* name;
            const char* text;
            const char* want_line;  // `文件:行:`
            const char* word;       // 报错里必须出现的词（修法 / 原因）
            const char* word2;      // 可空：第二条必须出现的词（`wait` 的文案要点名改写后的整行）
        };
        const StrayTimeout kStray[] = {
            {"crossline.plan", "await script TITLE\ntimeout 30000\nclick 1 2\n", ":2:", "末尾", nullptr},
            {"waitto.plan", "wait 5000 timeout 9000\n", ":1:", "固定时长", "写回 `wait 5000`"},
            {"clickto.plan", "click 1186 373 timeout 5000\n", ":1:", "没有可等待的东西", nullptr},
            {"quitto.plan", "quit timeout 5000\n", ":1:", "没有可等待的东西", nullptr},
        };
        for (const StrayTimeout& s : kStray) {
            const fs::path p = root / s.name;
            {
                std::ofstream out(p, std::ios::binary);
                out << s.text;
            }
            const std::string tag = std::string(s.name) + ".";
            const ChildRun r = runChild(exe, L"--input-plan " + quoteArg(p.wstring()), root, root / (tag + "out"),
                                        root / (tag + "err"));
            LIFT_CHECK(r.started);
            LIFT_CHECK(!r.timed_out);
            LIFT_CHECK_EQ_U32(r.exit_code, 2u);
            LIFT_CHECK(r.err.find("未知参数") == std::string::npos);
            LIFT_CHECK(r.err.find(std::string(s.name) + s.want_line) != std::string::npos);
            LIFT_CHECK(r.err.find(s.word) != std::string::npos);
            if (s.word2 != nullptr) LIFT_CHECK(r.err.find(s.word2) != std::string::npos);
        }
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

    // ---- ②' `--scale`：垃圾值 / 缺值 ⇒ exit 2 且**点名参数**（⛔ 不是"未知参数"，⛔ 不静默当缺省）----
    //      红了 ⇒ 用户给了非法倍率却跑出一个静默的缺省窗口（"看起来生效了、其实没生效"）。
    g_current_case = "scale-bogus-values-rejected";
    {
        struct BadScale {
            const wchar_t* arg;  // `--scale` 之后那个值（宽字符，直接拼进子进程命令行）
            const char* word;    // stderr 里必须出现的词（值本身）
        };
        const BadScale kBads[] = {
            {L"0", "0"},     {L"-1", "-1"}, {L"nan", "nan"}, {L"inf", "inf"},
            {L"abc", "abc"}, {L"1.5x", "1.5x"},
        };
        for (const BadScale& b : kBads) {
            const std::wstring cmd = std::wstring(L"--scale ") + b.arg + L" --present headless";
            std::string tag = "scale-";
            for (const wchar_t* p = b.arg; *p != L'\0'; ++p) tag.push_back(static_cast<char>(*p));
            tag += ".";
            const ChildRun r = runChild(exe, cmd, root, root / (tag + "out"), root / (tag + "err"));
            LIFT_CHECK(r.started);
            LIFT_CHECK(!r.timed_out);
            LIFT_CHECK_EQ_U32(r.exit_code, 2u);
            LIFT_CHECK(r.err.find("未知参数") == std::string::npos);
            LIFT_CHECK(r.err.find("--scale") != std::string::npos);
            LIFT_CHECK(r.err.find(b.word) != std::string::npos);
            // 启动前拒绝 ⇒ 一行运行摘要都没有。
            LIFT_CHECK(!hasLineStartingWith(r.out, "stop"));
        }
        // 缺值：`--scale` 后面没有东西 ⇒ exit 2 + `需要一个值` + 点名前缀（与其它 flag 同一句）。
        const ChildRun missing =
            runChild(exe, L"--scale", root, root / "scalemiss.out", root / "scalemiss.err");
        LIFT_CHECK(missing.started);
        LIFT_CHECK(!missing.timed_out);
        LIFT_CHECK_EQ_U32(missing.exit_code, 2u);
        LIFT_CHECK(missing.err.find("--scale") != std::string::npos);
        LIFT_CHECK(missing.err.find("需要一个值") != std::string::npos);
    }

    // ---- ②'' `--scale` 在 headless 下**接受但不生效**（⛔ 不进"headless 不接受窗口参数"那一类）----
    //      ★ 不判总退出码：这条命令之后本臂仍会因"临时 cwd 里没有执行源 / 没有 GPU"exit 2 ——
    //        判据是"`--scale` 这一关过去了"，⛔ 不拿总退出码冒充。readout 那一行（`scale: …`）
    //        在 GPU bring-up 之后才打（本用例**不假定 GPU 在场**）⇒ 那一面由 pure 用例
    //        `window-scale/headless-readout` 钉。
    g_current_case = "scale-headless-accepted";
    {
        const ChildRun r = runChild(exe, L"--scale 1.5 --present headless", root, root / "scalehl.out",
                                    root / "scalehl.err");
        LIFT_CHECK(r.started);
        LIFT_CHECK(!r.timed_out);
        LIFT_CHECK_EQ_U32(r.exit_code, 2u);  // 仍会因别的原因退出；下面三条才是判据
        LIFT_CHECK(r.err.find("未知参数") == std::string::npos);
        LIFT_CHECK(r.err.find("--scale") == std::string::npos);           // 没有对 --scale 的报错
        // ⛔ 不是被当成窗口参数拒绝（判据是那句话，不是"出现过 --present headless"：
        //    无 GPU 的机器上拒绝启动的文案里**也会**提到 `--present headless`）。
        LIFT_CHECK(r.err.find("不接受窗口参数") == std::string::npos);
        // 对照组：`auto` 同样被接受（数值与 auto 走同一个分支）。
        const ChildRun ra = runChild(exe, L"--scale auto --present headless", root, root / "scaleauto.out",
                                     root / "scaleauto.err");
        LIFT_CHECK(ra.started);
        LIFT_CHECK(!ra.timed_out);
        LIFT_CHECK(ra.err.find("未知参数") == std::string::npos);
        LIFT_CHECK(ra.err.find("--scale") == std::string::npos);
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

    // ---- ④ `--sound`：**接受** + 一条一次性说明（与代码一致：真的进静音门），⛔ 不是 `未知参数` ----
    //      ★ 不判退出码：`--sound` 之后本臂仍会因"没有执行源 / 没有 GPU"而 exit 2 —— 那与
    //        `--sound` 无关，判据是"它被接受 + 说了实话"，在 GPU bring-up 之前就已成立。
    //      ★ **洞 6**：这条用例**不再**拿"说明文本里含 `muted=0`"当证据（自证式断言：接线丢了它照绿）。
    //        证据 = `aot-audio-wiring` 读收口摘要的 `audio     : … muted=0`（含 `muted=1` 对照组）。
    //        这里保留的文本判据只有一条**反向**的：⛔ 不许把接线前的谎话写回来。
    g_current_case = "sound-accepted-with-once-notice";
    {
        const ChildRun r = runChild(exe, L"--sound", root, root / "sound.out", root / "sound.err");
        LIFT_CHECK(r.started);
        LIFT_CHECK(!r.timed_out);
        LIFT_CHECK(r.err.find("未知参数") == std::string::npos);
        LIFT_CHECK(countOccurrences(r.err, kSoundNoticeMarker) == 1u);
        // 反向判据：⛔ 不许再写回"没有音频管线 / 不会有声音"（接线前的形状）。
        LIFT_CHECK(r.err.find("没有音频管线") == std::string::npos);
        LIFT_CHECK(r.err.find("不会有声音") == std::string::npos);
        // ③ 跟踪单 id 必须**是真的**（`REQ-` + 26 位 ULID），⛔ 不许把占位符发出去。
        //    ★ 按**形态**判、不锚具体 id：换单号不该让守卫红，但留着 `TODO(` 必须红。
        LIFT_CHECK(r.err.find("REQ-") != std::string::npos);
        LIFT_CHECK(r.err.find("TODO(") == std::string::npos);
    }

    // ---- ⑥ S1：`--audio virtual` / `--audio sdl` **不再**被拒（旧文案"本臂只有 off"必须消失）----
    //      ★ 仍然 exit 2 是**别的**原因（临时 cwd 里没有执行源 / 没有 GPU）—— 判据只判"音频这一关
    //        过去了没有"，⛔ 不拿总退出码冒充。
    g_current_case = "audio-backends-accepted";
    {
        for (const wchar_t* mode : {L"virtual", L"sdl"}) {
            const ChildRun r = runChild(exe, std::wstring(L"--audio ") + mode + L" --present headless", root,
                                        root / (std::wstring(mode) + L".out"), root / (std::wstring(mode) + L".err"));
            LIFT_CHECK(r.started);
            LIFT_CHECK(!r.timed_out);
            LIFT_CHECK(r.err.find("未知参数") == std::string::npos);
            // 旧形状：`--audio %s 未实现（本臂只有 off：没有音频设备 / 喂料线程）`。
            LIFT_CHECK(r.err.find("未实现（本臂只有 off") == std::string::npos);
            LIFT_CHECK(r.err.find("值非法") == std::string::npos);
        }
    }

    // ---- ⑥' 非法值**仍然** exit 2，且点名合法值域（⛔ 不许"认不出就当 off"）----
    g_current_case = "audio-bogus-value-rejected";
    {
        const ChildRun r = runChild(exe, L"--audio bogus --present headless", root, root / "bogus.out",
                                    root / "bogus.err");
        LIFT_CHECK(r.started);
        LIFT_CHECK(!r.timed_out);
        LIFT_CHECK_EQ_U32(r.exit_code, 2u);
        LIFT_CHECK(r.err.find("未知参数") == std::string::npos);
        LIFT_CHECK(r.err.find("--audio") != std::string::npos);
        LIFT_CHECK(r.err.find("bogus") != std::string::npos);
        // 合法值域必须**写在报错里**（人照它改），不是只说"非法"。
        LIFT_CHECK(r.err.find("off") != std::string::npos);
        LIFT_CHECK(r.err.find("virtual") != std::string::npos);
        LIFT_CHECK(r.err.find("sdl") != std::string::npos);
    }

    // ---- ⑥'' `--audio-dump` 缺值 ⇒ exit 2 + 点名这个 flag（⛔ 不静默当成"没有 dump"）----
    g_current_case = "audio-dump-needs-value";
    {
        const ChildRun r = runChild(exe, L"--audio-dump", root, root / "dumpmiss.out", root / "dumpmiss.err");
        LIFT_CHECK(r.started);
        LIFT_CHECK(!r.timed_out);
        LIFT_CHECK_EQ_U32(r.exit_code, 2u);
        LIFT_CHECK(r.err.find("--audio-dump") != std::string::npos);
        LIFT_CHECK(r.err.find("需要一个值") != std::string::npos);
        LIFT_CHECK(r.err.find("未知参数") == std::string::npos);
    }

    // ---- ⑦ `--profile-slow-frames`：**依赖 `--profile`**（缺了它 ⇒ exit 2 并点名两者）----
    //      红了 ⇒ 一个拿不到墙钟尺的仪器被静默接受（表里 `wall_ms` 全 0 / 表根本不出现），
    //      而那正是"以为在量、其实没量"。★ 值 0（显式关）同样要带 `--profile`：判据只有一条。
    g_current_case = "profile-slow-frames-requires-profile";
    {
        const ChildRun r = runChild(exe, L"--profile-slow-frames 50 --present headless", root, root / "psf.out",
                                    root / "psf.err");
        LIFT_CHECK(r.started);
        LIFT_CHECK(!r.timed_out);
        LIFT_CHECK_EQ_U32(r.exit_code, 2u);
        LIFT_CHECK(r.err.find("未知参数") == std::string::npos);
        LIFT_CHECK(r.err.find("--profile-slow-frames") != std::string::npos);
        LIFT_CHECK(r.err.find("同时给 --profile") != std::string::npos);
        // 启动前拒绝 ⇒ 一行运行摘要都没有（不是"跑了一部分才停"）
        LIFT_CHECK(!hasLineStartingWith(r.out, "stop"));
        const ChildRun r0 = runChild(exe, L"--profile-slow-frames 0 --present headless", root,
                                     root / "psf0.out", root / "psf0.err");
        LIFT_CHECK(r0.started);
        LIFT_CHECK(!r0.timed_out);
        LIFT_CHECK_EQ_U32(r0.exit_code, 2u);
        LIFT_CHECK(r0.err.find("未知参数") == std::string::npos);
        LIFT_CHECK(r0.err.find("--profile-slow-frames") != std::string::npos);
    }

    // ---- ⑦' 取值：负数 / 小数 / 单位 / 垃圾 / 缺值 ⇒ exit 2 且**点名这个 flag** ----
    //      红了 ⇒ `strtoull("-1")` 式的静默回绕（负号被当成巨大毫秒数）或"认不出就当 0"
    //      （= 让人以为开着仪器、其实一次都不记）。
    g_current_case = "profile-slow-frames-bad-values";
    {
        struct BadSlow {
            const wchar_t* arg;
            const char* word;  // stderr 里必须出现的那个值（人照它改）
        };
        const BadSlow kBads[] = {{L"abc", "abc"}, {L"-1", "-1"}, {L"1.5", "1.5"}, {L"50ms", "50ms"}};
        for (const BadSlow& b : kBads) {
            const std::wstring cmd =
                std::wstring(L"--profile --profile-slow-frames ") + b.arg + L" --present headless";
            std::string tag = "slow-";
            for (const wchar_t* p = b.arg; *p != L'\0'; ++p) tag.push_back(static_cast<char>(*p));
            tag += ".";
            const ChildRun r = runChild(exe, cmd, root, root / (tag + "out"), root / (tag + "err"));
            LIFT_CHECK(r.started);
            LIFT_CHECK(!r.timed_out);
            LIFT_CHECK_EQ_U32(r.exit_code, 2u);
            LIFT_CHECK(r.err.find("未知参数") == std::string::npos);
            LIFT_CHECK(r.err.find("--profile-slow-frames") != std::string::npos);
            LIFT_CHECK(r.err.find(b.word) != std::string::npos);
        }
        // 缺值：`--profile-slow-frames` 后面没有东西 ⇒ exit 2 + `需要一个值` + 点名（与其它 flag 同一句）。
        const ChildRun missing = runChild(exe, L"--profile --profile-slow-frames", root, root / "slowmiss.out",
                                          root / "slowmiss.err");
        LIFT_CHECK(missing.started);
        LIFT_CHECK(!missing.timed_out);
        LIFT_CHECK_EQ_U32(missing.exit_code, 2u);
        LIFT_CHECK(missing.err.find("--profile-slow-frames") != std::string::npos);
        LIFT_CHECK(missing.err.find("需要一个值") != std::string::npos);
    }

    // ---- ⑦'' `--profile --profile-slow-frames 50` **被接受**（⛔ 不是 `未知参数`）----
    //      ★ 不判总退出码：之后本臂仍会因"临时 cwd 里没有执行源 / 没有 GPU"exit 2 ——
    //        判据是"这一关过去了"，⛔ 不拿总退出码冒充（与 `scale-headless-accepted` 同一条处置）。
    g_current_case = "profile-slow-frames-accepted-with-profile";
    {
        const ChildRun r = runChild(exe, L"--profile --profile-slow-frames 50 --present headless", root,
                                    root / "slowok.out", root / "slowok.err");
        LIFT_CHECK(r.started);
        LIFT_CHECK(!r.timed_out);
        LIFT_CHECK(r.err.find("未知参数") == std::string::npos);
        LIFT_CHECK(r.err.find("--profile-slow-frames") == std::string::npos);
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
