// emulator-lift · **assets 档**端到端守卫：运行摘要的 `present-tail` 行
//
// 口径（这一行回答什么）：**最后一次 Present 调用 → 本次 run 停**之间的 guest 指令数。
// 红了意味着什么（判据，缺一条都不算）：
//   ① 两次子进程（A / B）都 **exit 0**；
//   ② 两份日志都有 `[input-script] finished` —— 脚本真被跑完（否则"什么都没发生就退出"也能绿）；
//   ③ 两份日志各**恰好 1 行** `^present-tail: `（0 行 = 本应 Present 却没打 / 一行都没 Present；
//      2 行 = 摘要打了两次）；
//   ④ 每一份都满足恒等式 `insns_at_last_present + tail_insns == insns`（`insns :` 那一行的值）。
//      ⚠ **这条很弱**：`tail_insns := insns − insns_at_last_present` ⇒ 只要章 ≤ `insns`，恒等式**恒真**
//      （实测：把**盖章字段本身**换成 `r.ea`、`tail_insns` 由它推导时，本用例仍 24/0 绿）⇒ 它**只**抓
//      "章 > `insns`（clamp 触发）"这一种错接。**字段接错的真正抓点是纯档 `tests/input_injection.cpp`
//      的 `present-tail-stamp`**（它要求 `insns_at_last_present` 与 shim 内记下的 `cpu.insns` 精确相等）。
//   ⑤ 每一份都满足存在性 `last_frame >= 1` ∧ `insns_at_last_present > 0` ∧ `tail_insns > 0`
//      ∧ `tail_wall_ms > 0`（章没盖 / 停在最后一次 Present 内部 / 尾段其实是 0）；
//   ⑥ 两次运行的 `present-tail` 前缀（截到 `tail_wall_ms=` **之前**）**逐字相同**
//      —— `last_frame` / `insns_at_last_present` / `tail_insns` 三个数跨运行一致 = **确定性**
//      （末尾再单独断言一次 `tail_insns` 相等，让这条判据在失败信息里可读）。
//      ⛔ 不断言 `tail_wall_ms`（宿主墙钟，只能当单样本）。
//   ⑦ **锚真的命中 TITLE 的 Quit**（`REQ-01M745ZGKZAHTCEQ9KQ7CY3JJ0`）：两份日志各**恰好 1 行**
//      `[input-script] … click-release at=(1190,630) …`，并且
//      · `frame >= 1` —— 抬起时**已经 Present 过**（⇒ 不在 boot 里；boot 里 frame=0）；
//      · `waited_samples > 0` —— 自动抬起真的等到了 guest 的输入采样（⇒ guest 看见了这次按下）；
//      · **没有** `bounded=1` —— 不是"到 `--input-max-hold` 上限强行抬起"（= 一次采样都没有）。
//      任意一条不成立 ⇒ 打 `锚未命中（n=0）` 并判红（⛔ 不许退化成墙钟、也不许被 `--time-limit` 兜底成通过）。
//   ⑧ 两份日志的 `stop      :` 必须是 **`stopped`**（⛔ 不是 `time-limit`），且 `detail    :` 含
//      `kernel32!ExitProcess` —— "Quit 真的被点到、引擎自己退出"的停因证据（墙钟兜底会给 `time-limit`）。
//
// ★ 判据的**盲区**（诚实登记，⛔ 不要为它造量级断言）：`tail_wall_ms` 的**单位换算**没有守卫
//   （`ms → µs` 改坏之后全 ctest 仍绿 —— 它只被当成"一个 > 0 的数"读）。
//   `--max-frames ⇒ tail_insns == 0` 同理：只有 README 的一句"实测、无用例钉"，没有用例。
//
// ★ **为什么不断言 `tail_insns` 的某个字面值**（`REQ-01M745ZGKZAHTCEQ9KQ7CY3JJ0` 允许的第二种写法）：
//   它是**读数**且随合法改动变（例：`native_override_e2e` 给尾段的 LZSS 加 override 后它必然变小）
//   ⇒ 钉死它会让那些改动**红在错的原因上**（AGENTS.md §10）。本用例的确定性判据是
//   **两次运行逐字相同**（⑥）+ **停因 stopped/ExitProcess**（⑧）+ **锚命中**（⑦）：
//   "锚退化成墙钟"恰好同时破坏这三者（实测：`waited_samples=0` / `bounded=1` / `stop: time-limit`）。
//   本机读数（两次 run 逐字相同）：`tail_insns=131104827`，见
//   `.tmp/reveq/REQ-01M745ZGKZAHTCEQ9KQ7CY3JJ0/report.md`。
//
// ★ 为什么是 `--present headless --raster gpu --audio off`（可复现档）：无 sink ⇒ 不 pump 宿主事件、
//   虚拟时钟、没有音频 feeder 线程；GPU 光栅化是**宿主**路径，不改 guest 指令流。
//   ⛔ 不加 `--max-frames`（停点在 Present 内部 ⇒ 那一行恒为 0），⛔ 不加 `--bench`。
// ★ **输入锚 = guest 时间**（`REQ-01M745ZGKZAHTCEQ9KQ7CY3JJ0`）：`at frame 130` 在 boot 的**帧停滞**
//   里会被折算成墙钟触发（实测 `帧停滞 30001ms（n=0）⇒ 剩余 frame 触发条件…折算成 wall_ms`），
//   于是 click 落在 boot 里（`frame=0`）、guest 一次都没采样（`waited_samples=0` / `bounded=1`）、
//   Quit 点不到 ⇒ 两轮都停在 `--time-limit 600` ⇒ 判据数由墙钟停点决定（跨运行不确定）。
//   ⛔ 帧号不是时钟：boot 有没有 Present、帧停在哪，都取决于加载路径与宿主速度。
//   guest 时间（缺省 = 每条指令 100 ns）在同一代码路径上**跨机器确定** ⇒ 用它当锚。
// ★ **为什么是 `120000` ms 的余量**（读数见 `.tmp/reveq/REQ-01M745ZGKZAHTCEQ9KQ7CY3JJ0/report.md`）：
//   · Quit 点在 **frame 130** 那一刻稳定可点：实测 `guest_ms=87991` / `insns=878803180`、`waited_samples=3`
//     ⇒ `stop: stopped` + `kernel32!ExitProcess` + `tail_insns=131104827`（本机两次 run 逐字相同）。
//   · 锚必须**盖住跨机器 / 跨构建的 boot 差异**：同一脚本、同一末帧，boot 段实测差过
//     **73,750,950 条指令（= 7.375 s guest，macOS vs win）** ⇒ 只在 87991 上加 2 s（`90000`）余量不够。
//   · `120000` = Quit 点（87991）之后 **+32 s**（≈ 4.3 倍的实测 boot 差），且 `--time-limit 600`
//     （墙钟）离它还很远（本机 run 到 ExitProcess 的墙钟是 ~2 min 量级）⇒ 有余量、也不拖长用例。
//   · TITLE 是**静止的**（不自动进演示 / 不自动退出）：修前那份 `b` 日志在 boot 里点空之后，
//     引擎一直 Present 到 `last_frame=14957` 才被 `--time-limit` 收掉 ⇒ 晚 32 s 仍是同一屏。
// ★ 两次运行**只有 `--appdata` 不同**（各自私有、**且每次都清空**）：
//   共用一份会让第二次看到第一次留下的状态；**不清空**同样不行 —— 退出路径会往 appdata 写
//   `SAVE.DAT` / `RT.DAT`，下一次 run 读到旧存档就走另一条 boot/尾段（实测 `tail_insns`
//   131104827 vs 143751706；a/b 存档内容不同时两次 run 还会差一帧）。⇒ 见循环里的 `remove_all`。
// ★ GPU / 执行源不可用 ⇒ `LIFT_SKIP`(77)：那是**环境不满足**，不是判据红。
//   `--raster gpu` 需要 SDL_GPU：先按 `tests/gpu_raster.cpp` 的判据在本进程里探一次设备。
// ★ 子进程输出走**文件重定向**（⛔ 不走管道：受限沙箱里捕获管道会 EPERM）。
//   Windows 分支与 `tests/title_entry_paths.cpp` 同形（`CreateProcessW`）；POSIX 分支只为让这条
//   守卫在 macOS 上**也能真跑**（AGENTS.md：macOS 侧做兼容性验证）。两条分支都只做一件事：
//   起子进程、把 stdout+stderr 落到一个文件、拿退出码。
// ★ **命令行的路径编码**（`REQ-01M73BWF51T1SKSSE6PV6X455A`）：Windows 分支把参数拼成
//   **UTF-16**（`CreateProcessW`），路径一律 `fs::path::wstring()`；⛔ 不许 `path::string()`
//   —— 那走 ANSI 代码页（本机 936），`天結` 会被转坏、再按 UTF-8 解就成空串
//   （子进程收到 `--game-dir ""`）。边界口径与 `main` 的 `lift::argvUtf8()` 同源：
//   主程序只看 UTF-8 / UTF-16，⛔ 不引入第三种约定。
// ★ **交付配置下本文件只在 Windows 的资产块里注册**（`CMakeLists.txt` 的
//   `if(EXISTS "E:/Games/Eushully/天結/install/SYS4INI.BIN" AND DEFINED ENV{LIFT_ASSETS})`）⇒
//   macOS / Linux 上不注册、不编译。POSIX 分支 + `LIFT_ASSETS_GAME_DIR` 覆盖的用途是：
//   在 macOS 上**手工编译**跑同一条判据（本仓 macOS 只做兼容性验证、不阻塞）。
//   `LIFT_ASSETS_GAME_DIR` 只覆盖 game-dir（缺省仍用 CTest 传的 `E:/Games/Eushully/天結/install`），
//   ⛔ 不覆盖档位 / 脚本 / 断言。
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
#include "runtime/host_encoding.h"
#include "runtime/present_sdl.h"

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
#endif

namespace {

namespace fs = std::filesystem;

// 脚本：TITLE 的 Quit 在 (1190,630)（与 `.tmp/quit-tail/` 里那几份实测脚本同形）。
// ⛔ 不要在末尾加 `quit` / `wait`：本守卫要的是"引擎自己走到进程退出"的那一段尾段。
// ★ 锚**只写一处**：脚本与"锚未命中"的诊断文字都从 `kGuestAnchorMs` 拼出来
//   （⛔ 不许两处各写一个数字 —— 那会让诊断说的是旧锚）。
constexpr const char* kGuestAnchorMs = "120000";
const std::string kScript = std::string("at guest_ms ") + kGuestAnchorMs + "\nclick 1190 630\n";

struct ChildResult {
    int exit_code = -1;
    bool started = false;
};

std::string readAllBytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

#if defined(_WIN32)
std::wstring quoteArg(const std::wstring& s) { return L"\"" + s + L"\""; }

// stdout 与 stderr 都重定向到**同一个**日志文件；不捕获管道。
ChildResult runChild(const fs::path& exe, const std::wstring& args, const fs::path& log_path) {
    ChildResult r;
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
    std::wstring cmd = quoteArg(exe.wstring()) + L" " + args;
    const BOOL ok = ::CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                     nullptr, &si, &pi);
    ::CloseHandle(h);
    if (!ok) return r;
    r.started = true;
    // 每次 run 给 12 分钟（两次总和 < CTest 的 TIMEOUT 1800）；卡住必须红，⛔ 不许挂死流水线。
    const DWORD waited = ::WaitForSingleObject(pi.hProcess, 720000u);
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
// POSIX：只有一个 "把 stdout+stderr 落到一个文件" 的 shell 重定向。超时交给 CTest 的 TIMEOUT。
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
    if (rc == -1) return r;
    if (!fs::exists(log_path)) return r;  // 连日志都没写 ⇒ 根本没起来
    r.started = true;
    r.exit_code = WIFEXITED(rc) ? WEXITSTATUS(rc) : (WIFSIGNALED(rc) ? 128 + WTERMSIG(rc) : 1);
    return r;
}
#endif

// `text` 里以 `prefix` 开头的那一整行（没有 ⇒ 空串）
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

long countLinesStartingWith(const std::string& text, const std::string& prefix) {
    long n = 0;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t nl = text.find('\n', pos);
        const std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (line.compare(0, prefix.size(), prefix) == 0) ++n;
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return n;
}

// `text` 里含 `needle` 的第一整行（没有 ⇒ 空串）
std::string lineContaining(const std::string& text, const std::string& needle) {
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t nl = text.find('\n', pos);
        const std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (line.find(needle) != std::string::npos) return line;
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return {};
}

long countLinesContaining(const std::string& text, const std::string& needle) {
    long n = 0;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t nl = text.find('\n', pos);
        const std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (line.find(needle) != std::string::npos) ++n;
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return n;
}

bool contains(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

bool startsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

// 环境不可用（**不是**判据红）：GPU / 执行源。判据照 `tests/title_entry_paths.cpp` 的
//   `environmentUnavailable()`，**补上 headless 档的那条串**（本用例是 `--present headless` ，
//   那条走 `src/main.cpp` 的 `--raster gpu（headless）失败`，不是 window 档的「要求 GPU 光栅化」）。
//   ⚠ 不要匹配 `game-dir` —— 那行每个正常启动都会打印（会把任何非 0 退出都误判成环境问题）。
bool environmentUnavailable(const std::string& log) {
    return contains(log, "要求 GPU 光栅化")                       // window 档（title_entry_paths 用这条）
           || contains(log, "--raster gpu（headless）失败")        // headless 档：拿不到 SDL_GPU 设备
           || contains(log, "窗口打开失败") || contains(log, "找不到执行源") || contains(log, "拒绝启动");
}

// 从 `line` 里取 `<key>=<十进制>`；取不到 ⇒ false（⛔ 不拿默认值冒充）。
bool readU64(const std::string& line, const char* key, std::uint64_t* out) {
    const std::string k = std::string(key) + "=";
    const std::size_t at = line.find(k);
    if (at == std::string::npos) return false;
    const char* begin = line.c_str() + at + k.size();
    char* end = nullptr;
    const unsigned long long v = std::strtoull(begin, &end, 10);
    if (end == begin) return false;
    *out = static_cast<std::uint64_t>(v);
    return true;
}

bool readDouble(const std::string& line, const char* key, double* out) {
    const std::string k = std::string(key) + "=";
    const std::size_t at = line.find(k);
    if (at == std::string::npos) return false;
    const char* begin = line.c_str() + at + k.size();
    char* end = nullptr;
    const double v = std::strtod(begin, &end);
    if (end == begin) return false;
    *out = v;
    return true;
}

// 一份日志里 `present-tail` 的解析结果。
struct Tail {
    long count = 0;              // `^present-tail: ` 的行数（判据 ③：必须恰好 1）
    std::string prefix;          // 截到 `tail_wall_ms=` **之前**（判据 ⑥：两次逐字相同）
    std::uint64_t last_frame = 0;
    std::uint64_t insns_at_last_present = 0;
    std::uint64_t tail_insns = 0;
    double tail_wall_ms = 0.0;
    bool parsed = false;         // 4 个字段都取到了
    std::uint64_t insns = 0;     // 摘要 `insns     :` 行
    bool have_insns = false;
};

Tail parseTail(const std::string& log) {
    Tail t;
    t.count = countLinesStartingWith(log, "present-tail: ");
    const std::string line = lineStartingWith(log, "present-tail: ");
    if (!line.empty()) {
        const std::size_t cut = line.find("tail_wall_ms=");
        t.prefix = cut == std::string::npos ? line : line.substr(0, cut);
        const bool ok = readU64(line, "last_frame", &t.last_frame) &&
                        readU64(line, "insns_at_last_present", &t.insns_at_last_present) &&
                        readU64(line, "tail_insns", &t.tail_insns) &&
                        readDouble(line, "tail_wall_ms", &t.tail_wall_ms);
        t.parsed = ok && cut != std::string::npos;
    }
    // `insns     : %llu`（标签补空格到 10 列再 ": "，与 `insns :` 那一行逐字一致）
    const std::string insns_line = lineStartingWith(log, "insns     : ");
    if (!insns_line.empty()) {
        const char* begin = insns_line.c_str() + std::strlen("insns     : ");
        char* end = nullptr;
        const unsigned long long v = std::strtoull(begin, &end, 10);
        if (end != begin) {
            t.insns = static_cast<std::uint64_t>(v);
            t.have_insns = true;
        }
    }
    return t;
}

// 一份日志里 `[input-script] … click-release at=(1190,630) …` 的解析结果（判据 ⑦ 的输入）。
//   ★ 只认**抬起**那一行：`click-release scheduled: …` 是"排队"（不是抬起），
//     `[input-script] frame=… click 1190 630` 是动作本身（没有 `waited_samples`）。
struct Click {
    long count = 0;               // 这样的行数（判据 ⑦：必须恰好 1）
    bool parsed = false;          // frame / guest_ms / waited_samples 都取到
    std::uint64_t frame = 0;      // 抬起时已经 Present 过多少帧（**0 = 还在 boot**）
    std::uint64_t guest_ms = 0;   // 抬起时的 guest 时间（诊断用）
    std::uint64_t waited_samples = 0;
    bool bounded = false;         // `bounded=1` = 到 `--input-max-hold` 上限强行抬起（一次采样都没有）
};

Click parseClick(const std::string& log) {
    Click c;
    c.count = countLinesContaining(log, " click-release at=(");
    const std::string line = lineContaining(log, " click-release at=(");
    if (!line.empty()) {
        c.parsed = readU64(line, "frame", &c.frame) && readU64(line, "guest_ms", &c.guest_ms) &&
                   readU64(line, "waited_samples", &c.waited_samples);
        c.bounded = contains(line, "bounded=1");
    }
    return c;
}

// `stop      : stopped @ eip=…` 的**值**（停因名字，截到第一个空格）；行不在 ⇒ 空串。
std::string stopReason(const std::string& log) {
    const std::string line = lineStartingWith(log, "stop      : ");
    if (line.empty()) return {};
    const std::string rest = line.substr(std::strlen("stop      : "));
    const std::size_t sp = rest.find(' ');
    return sp == std::string::npos ? rest : rest.substr(0, sp);
}

// `detail    : …` 那一行；行不在 ⇒ 空串。
std::string stopDetail(const std::string& log) { return lineStartingWith(log, "detail    : "); }

}  // namespace

int main(int argc, char** argv) {
    using namespace lift_test;
    g_current_case = "quit-tail";

    const std::vector<std::string> args = lift::argvUtf8(argc, argv);
    if (args.size() < 4) {
        std::fprintf(stderr, "用法：lift-quit-tail <emulator-lift> <game-dir> <work-dir>\n");
        return 2;
    }
    const fs::path exe = fs::path(lift::utf8ToWide(args[1]));
    // game-dir 优先级：`LIFT_ASSETS_GAME_DIR`（env，macOS 手工跑用）> argv[2]（CTest 传的
    //   `E:/Games/Eushully/天結/install`）。★ env **只**覆盖 game-dir，档位 / 脚本 / 断言不变。
    fs::path game_dir = fs::path(lift::utf8ToWide(args[2]));
    if (const char* env_game = std::getenv("LIFT_ASSETS_GAME_DIR"); env_game != nullptr && *env_game != '\0') {
        game_dir = fs::path(lift::utf8ToWide(env_game));
        // ★ 日志展示也走 UTF-8：`path::string()` 会把 `天結` 按 ANSI 码页写成**非法 UTF-8** 字节。
        const std::string game_dir_utf8 = lift::wideToUtf8(game_dir.wstring().c_str());
        std::printf("quit-tail: game-dir 被 LIFT_ASSETS_GAME_DIR 覆盖 ⇒ %s\n", game_dir_utf8.c_str());
    }
    const fs::path work_dir = fs::path(lift::utf8ToWide(args[3]));
    LIFT_CHECK(fs::exists(exe));
    if (!fs::exists(exe)) return finish("quit-tail");

    // `--raster gpu` 需要 SDL_GPU 设备：按 `tests/gpu_raster.cpp` 的判据先探一次。
    //   ⛔ 拿"CPU 光栅化也能跑"冒充通过：那会把档位悄悄降级成非本守卫要测的路径。
    {
        lift::SdlWindow probe;
        lift::SdlWindowOptions popt;
        popt.title = "emulator-lift quit-tail gpu probe";
        popt.driver_list = "gpu";
        popt.hidden = true;
        popt.raster_gpu = true;
        std::string perr;
        if (!probe.init(popt, &perr)) LIFT_SKIP(perr.c_str());
        if (!probe.rasterActive()) {
            probe.shutdown();
            LIFT_SKIP("rasterActive == false（拿不到 SDL_GPU 设备 ⇒ 不是判据红）");
        }
        probe.shutdown();
    }

    std::error_code ec;
    fs::create_directories(work_dir, ec);
    LIFT_CHECK(!ec);
    const fs::path script = work_dir / "script-quit.txt";
    {
        std::ofstream out(script, std::ios::binary | std::ios::trunc);
        out << kScript;
        out.close();
        LIFT_CHECK(fs::exists(script));
    }

    // 两次**逐字相同**的命令，只有 `--appdata` 是各自私有的（见文件头）。
    Tail tails[2];
    for (int i = 0; i < 2; ++i) {
        const std::string tag = i == 0 ? "a" : "b";
        const fs::path appdata = work_dir / ("appdata-" + tag);
        // ★ `--appdata` 必须**每次都是空的**（文件头"各自私有、全新"这条原来只是注解，
        //   代码只 `create_directories` ⇒ 上一次 run 留下的存档会被下一次读到）。退出路径会
        //   往这里写 `SAVE.DAT` / `RT.DAT`，而"有没有存档 / 存档内容"会改 boot 与尾段的长度：
        //   实测（2026-11-11，同一份二进制、同一个 `at guest_ms 120000`）：
        //   · 空的 appdata：`tail_insns=131104827`；
        //   · 带上一轮写下的 48016 B 存档：`tail_insns=143751706`，且 a/b 两份存档一旦内容
        //     不同（`SAVE.DAT` sha256 实测 a≠b），两次 run 就会差一帧 ⇒ ⑥ 假红。
        //   ⇒ `remove_all` 再建；清不掉就当场红（⛔ 不许带着旧存档继续跑）。
        fs::remove_all(appdata, ec);
        LIFT_CHECK(!ec);
        fs::create_directories(appdata, ec);
        LIFT_CHECK(!ec);
        const fs::path log_path = work_dir / ("quit-tail-" + tag + ".log");
        fs::remove(log_path, ec);

        // ★ 路径在**边界上**的编码口径（`REQ-01M73BWF51T1SKSSE6PV6X455A`）：子进程的 `--game-dir`
        //   必须与 `main` 解析它的那一套**同源** —— `main` 收的是 `lift::argvUtf8()` 的 **UTF-8**
        //   （Windows 上由 `GetCommandLineW` 的 UTF-16 重建，见 `runtime/host_encoding.h`）。
        //   ⇒ Windows 上命令行**只用原始 UTF-16**（`CreateProcessW`），路径一律 `.wstring()`。
        //   ⛔ 不许 `path::string()`：它走 **ANSI 代码页**（本机 936）⇒ `天結` 变成 GBK 字节，
        //   再经 `lift::utf8ToWide()` 的**严格**解码就成了空串 ⇒ 子进程收到 `--game-dir ""`
        //   （实测症状：子进程日志首行「没有 --game-dir」，本用例 12 checks / 7 failures）。
#if defined(_WIN32)
        std::vector<std::wstring> child;
        child.push_back(L"--game-dir"), child.push_back(game_dir.wstring());
        child.push_back(L"--appdata"), child.push_back(appdata.wstring());
        child.push_back(L"--skip-logo");
        child.push_back(L"--audio"), child.push_back(L"off");
        child.push_back(L"--present"), child.push_back(L"headless");
        child.push_back(L"--raster"), child.push_back(L"gpu");
        child.push_back(L"--time-limit"), child.push_back(L"600");
        child.push_back(L"--input-script"), child.push_back(script.wstring());
        std::wstring wargs;
        for (const std::wstring& a : child) wargs += L" " + quoteArg(a);
        const ChildResult r = runChild(exe, wargs, log_path);
#else
        // POSIX：`path::string()` 就是本机原生编码（Linux / macOS = UTF-8），`system()` 原样进 argv。
        std::vector<std::string> child;
        child.push_back("--game-dir"), child.push_back(game_dir.string());
        child.push_back("--appdata"), child.push_back(appdata.string());
        child.push_back("--skip-logo");
        child.push_back("--audio"), child.push_back("off");
        child.push_back("--present"), child.push_back("headless");
        child.push_back("--raster"), child.push_back("gpu");
        child.push_back("--time-limit"), child.push_back("600");
        child.push_back("--input-script"), child.push_back(script.string());
        const ChildResult r = runChild(exe, child, log_path);
#endif
        if (!r.started) LIFT_SKIP("起不了 emulator-lift 子进程（exe 路径 / 沙箱限制？）");
        const std::string log = readAllBytes(log_path);
        if (log.empty()) LIFT_SKIP("子进程没写日志（工作目录 / 权限问题）");
        std::printf("quit-tail: run=%s exit=%d log=%s (%zu 字节)\n", tag.c_str(), r.exit_code,
                    log_path.string().c_str(), log.size());
        // ★ 环境豁免必须排在**第一个 `LIFT_CHECK` 之前**：`LIFT_SKIP` 在 `g_failures > 0` 时
        //   会 `return 1`（`tests/check.h`），排在断言之后就变成"环境问题报 FAIL"。
        if (r.exit_code != 0 && environmentUnavailable(log)) {
            LIFT_SKIP("GPU / 执行源不可用 ⇒ 这条不是判据红");                        // 同 title_entry_paths
        }
        tails[i] = parseTail(log);
        if (!tails[i].parsed) std::printf("quit-tail: run=%s 原始 present-tail 行：%s\n", tag.c_str(),
                                          lineStartingWith(log, "present-tail: ").c_str());
        else
            std::printf("quit-tail: run=%s %s\n", tag.c_str(), lineStartingWith(log, "present-tail: ").c_str());

        const Click click = parseClick(log);
        const std::string stop = stopReason(log);
        const std::string detail = stopDetail(log);
        // ★ ⑦ 的读数先打出来（红了 / 写报告时要能直接引用；⛔ 别去 grep 子进程日志猜）。
        std::printf("quit-tail: run=%s anchor click frame=%llu guest_ms=%llu waited_samples=%llu%s · stop=%s\n",
                    tag.c_str(), static_cast<unsigned long long>(click.frame),
                    static_cast<unsigned long long>(click.guest_ms),
                    static_cast<unsigned long long>(click.waited_samples), click.bounded ? " bounded=1" : "",
                    stop.c_str());

        // ⚠ `g_current_case` 是 `const char*` ⇒ 名字要存在 static 里（⛔ 不能指向临时 string）
        static std::string case_name;
        case_name = std::string("quit-tail/") + tag;
        g_current_case = case_name.c_str();
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(r.exit_code), 0u);              // ①
        LIFT_CHECK(contains(log, "[input-script] finished"));                        // ②
        LIFT_CHECK_EQ_I64(tails[i].count, 1);                                        // ③ 恰好 1 行
        LIFT_CHECK(tails[i].parsed);                                                 // 4 个字段都解析出来
        LIFT_CHECK(tails[i].have_insns);                                             // `insns     :` 也在
        if (tails[i].parsed && tails[i].have_insns) {
            // ④ 恒等式（⚠ 只抓"章 > insns / clamp 触发"这一种错接；字段接错靠纯档 present-tail-stamp）
            LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(tails[i].insns_at_last_present + tails[i].tail_insns),
                              static_cast<std::int64_t>(tails[i].insns));
        }
        LIFT_CHECK(tails[i].last_frame >= 1);                                        // ⑤ 存在性
        LIFT_CHECK(tails[i].insns_at_last_present > 0);                              //   章真的盖了
        LIFT_CHECK(tails[i].tail_insns > 0);                                         //   确实有尾段
        LIFT_CHECK(tails[i].tail_wall_ms > 0.0);                                     //   墙钟不是 0

        // ⑦ 锚真的命中 TITLE 的 Quit：`click-release` 行存在 / 抬起时已 Present 过（不在 boot）/
        //   自动抬起等到了 guest 的输入采样 / 不是 `--input-max-hold` 上限兜底。
        //   任意一条不成立 ⇒ **锚未命中（n=0）**，响亮失败（⛔ 不退化到墙钟、⛔ 不许 time-limit 兜底成通过）。
        case_name = std::string("quit-tail/") + tag + "/anchor-hit";
        g_current_case = case_name.c_str();
        const bool anchor_hit = click.count == 1 && click.parsed && click.frame >= 1 &&
                                click.waited_samples > 0 && !click.bounded;
        LIFT_CHECK_EQ_I64(click.count, 1);            // 恰好 1 行 `click-release at=(…)`
        LIFT_CHECK(click.parsed);                     // frame / guest_ms / waited_samples 都在
        LIFT_CHECK(click.frame >= 1);                 // 抬起时已经 Present 过（boot 里是 0）
        LIFT_CHECK(click.waited_samples > 0);         // guest 真的采样到了这次按下
        LIFT_CHECK(!click.bounded);                   // 不是"到 --input-max-hold 上限强行抬起"
        if (!anchor_hit) {
            std::fprintf(stderr,
                         "quit-tail: run=%s 锚未命中（n=0）—— `at guest_ms %s` 没落到 TITLE 的 Quit 上："
                         "click-release 行=%ld · frame=%llu · waited_samples=%llu · bounded=%d。"
                         "⛔ 不退化到墙钟，也不许由 --time-limit 兜底成通过。\n",
                         tag.c_str(), kGuestAnchorMs, click.count, static_cast<unsigned long long>(click.frame),
                         static_cast<unsigned long long>(click.waited_samples), click.bounded ? 1 : 0);
        }

        // ⑧ 停因：必须是**引擎自己退出**（stopped + `kernel32!ExitProcess`），⛔ 不是墙钟兜底 `time-limit`。
        case_name = std::string("quit-tail/") + tag + "/stop";
        g_current_case = case_name.c_str();
        LIFT_CHECK(startsWith(stop, "stopped"));
        LIFT_CHECK(contains(detail, "kernel32!ExitProcess"));
        if (g_failures > 0) break;  // 第一份就红 ⇒ 第二份的读数也不该被当真
    }

    // ⑥ 跨运行：`last_frame` / `insns_at_last_present` / `tail_insns` 三个数必须逐字相同
    //   （只截到 `tail_wall_ms=` **之前** ⇒ 宿主墙钟不参与比较）。
    //   ★ 再单独断言一次 `tail_insns`：它是本用例最要紧的那个数（判据的可见性），
    //     失败信息里要能一眼看到两个值（前缀比较只在 stderr 打两行）。
    if (tails[0].parsed && tails[1].parsed) {
        g_current_case = "quit-tail/deterministic";
        if (tails[0].prefix != tails[1].prefix) {
            std::fprintf(stderr, "  run=a: %s\n  run=b: %s\n", tails[0].prefix.c_str(), tails[1].prefix.c_str());
        }
        LIFT_CHECK(tails[0].prefix == tails[1].prefix);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(tails[0].tail_insns),
                          static_cast<std::int64_t>(tails[1].tail_insns));
        std::printf("quit-tail: deterministic tail_insns=%llu（两次逐字相同）\n",
                    static_cast<unsigned long long>(tails[0].tail_insns));
    }

    return finish("quit-tail");
}
