// emulator-lift · **assets 档**端到端守卫：退出写存档尾段的 LZSS Encode native override
// （REQ-01M7375DARF1QFZEFNWX3CAK6T）
//
// 形制照 `tests/quit_tail.cpp`：同一份输入脚本、同一命令形状，**唯一的差异是
// `--native-overrides sub_4367B0`**。三份 run：`a1`（空）/ `b`（开）/ `a2`（空，重复一次）。
//
// 判据（红了意味着什么；缺一条都不算）：
//   ① 三份都 **exit 0**、都有 `[input-script] finished`、各**恰好 1 行** `^present-tail: `；
//   ② **主判据**：`b` 的 `native-ovr` 明细行里 `sub_4367B0` 必须 `hits==1 && fallbacks==0`
//      （同时排除"没启用"与"每次回退"两种"看起来变快了"），且 `a1` 里**没有** `native-ovr` 行；
//   ③ 两臂尾段 `tail_insns` **显著不同**：`b < a1/2`（⛔ 不断言具体数字 —— 那是读数）；
//   ④ 三份都 `stop=stopped` 且 `detail` 含 `ExitProcess`（⛔ 不是 `time-limit`：被兜底截断的 run 也小）；
//   ⑤ 尾段墙钟 `tail_wall_ms(b) < tail_wall_ms(a1)`：**只比更低**，⛔ 不给绝对阈值、不给倍数下限
//      （同一份二进制两次 run 的墙钟实测差到 1.48×，见 `.tmp/quit-tail/report.md` §1.3）。
//      它是最弱的一条（宿主抖动），所以放在最后、只当辅助读数；
//   ⑥ 存档差分：`a1` vs `a2` 的 `SAVE.DAT` **逐字节相同**（不需要掩码，抓"未初始化字节"这类真错）；
//      `a1` vs `b` 的差异**必须落在虚拟时钟区**里，且**打印实测区间**：
//        `kSaveClockOffset` / `kSaveClockSpan` 是**具名常量**，由落地后第一次两臂实测填
//        （⛔ 不抄任务单的 24 / `[264..284)`：两者冲突且都没实测；见 `.tmp/reveq/…/report.md`）。
// ★ 不断言任何当前数字（`tail_insns ≈ 1.3e8`、`comp` 之类）—— 它们是**读数**，会随构建变。
// ★ 环境不可用（没有游戏安装 / GPU / 执行源）⇒ `LIFT_SKIP`(77)：那不是判据红。
// ★ 子进程输出走**文件重定向**（⛔ 不走管道：受限沙箱里捕获管道会 EPERM）。
#include <algorithm>
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

// TITLE 的 Quit 在 (1190,630)；`profile_begin tail` 把尾段开成一个可读窗口（与报告里的实测脚本同形）。
// ⛔ 不要在末尾加 `quit` / `wait`：本守卫要的是"引擎自己走到进程退出"的那一段尾段。
constexpr const char* kScript = "at frame 130\nclick 1190 630\nat frame 150\nprofile_begin tail\n";

// ★ 具名常量：写存档里**虚拟时钟**的落点（`GetLocalTime` / `timeGetTime` 的字节区间）。
//   ★ 本机实测（2026-11-11，第一次两臂 run；见 `.tmp/reveq/REQ-01M7375DARF1QFZEFNWX3CAK6T/report.md`）：
//     `SAVE.DAT`（48128 B）a1-vs-b 只有 **4 个字节**不同：偏移 **276 / 278 / 279 / 280**（277 相同）
//     ⇒ 实测区间 = **[276..280]**，掩码取它的闭包 `[276, 281)`。
//     虚拟时钟差 = (131,104,827 − 8,495,927) × 100 ns ≈ **12.26 s**（尾段省掉的指令数）。
//     两臂同配置各自重复一次 ⇒ `SAVE.DAT` 逐字节相同（所以掩码**只**为时钟而存在）。
//   ⛔ 不要照抄任务单的 `24` / `[264..284)`（两者都没实测、且与实测不符）；
//     若将来这里红了而差异仍落在时间块内，**重新实测再改这两个数**，⛔ 不要放宽断言了事。
constexpr std::uint32_t kSaveClockOffset = 276u;
constexpr std::uint32_t kSaveClockSpan = 5u;

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
    // 每次 run 给 12 分钟（三份总和 < CTest 的 TIMEOUT 2400）；卡住必须红，⛔ 不许挂死流水线。
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
    if (!fs::exists(log_path)) return r;
    r.started = true;
    r.exit_code = WIFEXITED(rc) ? WEXITSTATUS(rc) : (WIFSIGNALED(rc) ? 128 + WTERMSIG(rc) : 1);
    return r;
}
#endif

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

// `text` 里**第一行**同时含 `a` 与 `b` 的整行（没有 ⇒ 空串）。
// ★ 用两个 needle 是必须的：`sub_4367B0` 这个名字在启动行（`  sub_4367B0   @ 004367B0 thiscall(8B) [debug]`）、
//   摘要明细行（带 `hits=`）、窗口 profile 行（带 `self=`）里都出现 ⇒ 单 needle 会抓错行。
std::string lineContaining2(const std::string& text, const std::string& a, const std::string& b) {
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t nl = text.find('\n', pos);
        const std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (line.find(a) != std::string::npos && line.find(b) != std::string::npos) return line;
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return {};
}

bool contains(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

bool environmentUnavailable(const std::string& log) {
    return contains(log, "要求 GPU 光栅化") || contains(log, "--raster gpu（headless）失败")
           || contains(log, "窗口打开失败") || contains(log, "找不到执行源") || contains(log, "拒绝启动");
}

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
    long count = 0;
    std::uint64_t last_frame = 0;
    std::uint64_t insns_at_last_present = 0;
    std::uint64_t tail_insns = 0;
    double tail_wall_ms = 0.0;
    bool parsed = false;
};

Tail parseTail(const std::string& log) {
    Tail t;
    t.count = countLinesStartingWith(log, "present-tail: ");
    const std::string line = lineStartingWith(log, "present-tail: ");
    if (!line.empty()) {
        t.parsed = readU64(line, "last_frame", &t.last_frame) &&
                   readU64(line, "insns_at_last_present", &t.insns_at_last_present) &&
                   readU64(line, "tail_insns", &t.tail_insns) && readDouble(line, "tail_wall_ms", &t.tail_wall_ms);
    }
    return t;
}

// 递归找 `<appdata>` 下唯一一个名为 `name` 的文件（存档目录名是日文，⛔ 不硬编码路径）。
fs::path findFile(const fs::path& root, const char* name) {
    std::error_code ec;
    fs::path found;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code fec;
        if (it->is_regular_file(fec) && it->path().filename() == fs::path(name)) {
            if (!found.empty()) return {};  // 不唯一 ⇒ 调用方按失败处理
            found = it->path();
        }
    }
    return found;
}

struct ByteDiff {
    bool valid = false;
    bool same_size = false;
    std::size_t count = 0;
    std::size_t first = 0;
    std::size_t last = 0;
};

ByteDiff diffBytes(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    ByteDiff d;
    d.valid = true;
    d.same_size = a.size() == b.size();
    if (!d.same_size) return d;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] == b[i]) continue;
        if (d.count == 0) d.first = i;
        d.last = i;
        ++d.count;
    }
    return d;
}

bool readBytesFile(const fs::path& p, std::vector<std::uint8_t>* out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace lift_test;
    g_current_case = "native-override-e2e";

    const std::vector<std::string> args = lift::argvUtf8(argc, argv);
    if (args.size() < 4) {
        std::fprintf(stderr, "用法：lift-native-override-e2e <emulator-lift> <game-dir> <work-dir>\n");
        return 2;
    }
    const fs::path exe = fs::path(lift::utf8ToWide(args[1]));
    fs::path game_dir = fs::path(lift::utf8ToWide(args[2]));
    if (const char* env_game = std::getenv("LIFT_ASSETS_GAME_DIR"); env_game != nullptr && *env_game != '\0') {
        game_dir = fs::path(lift::utf8ToWide(env_game));
        std::printf("native-override-e2e: game-dir 被 LIFT_ASSETS_GAME_DIR 覆盖 ⇒ %s\n",
                    game_dir.string().c_str());
    }
    const fs::path work_dir = fs::path(lift::utf8ToWide(args[3]));
    LIFT_CHECK(fs::exists(exe));
    if (!fs::exists(exe)) return finish("native-override-e2e");

    // `--raster gpu` 需要 SDL_GPU 设备：按 `tests/gpu_raster.cpp` 的判据先探一次。
    {
        lift::SdlWindow probe;
        lift::SdlWindowOptions popt;
        popt.title = "emulator-lift lzss override e2e gpu probe";
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
    }
    LIFT_CHECK(fs::exists(script));

    struct Arm {
        const char* tag;
        bool with_override;
    };
    const Arm arms[3] = {{"a1", false}, {"b", true}, {"a2", false}};
    Tail tails[3];
    std::string logs[3];
    std::vector<std::uint8_t> save[3];
    std::vector<std::uint8_t> rt[3];
    bool save_ok[3] = {false, false, false};
    bool rt_ok[3] = {false, false, false};

    for (int i = 0; i < 3; ++i) {
        const std::string tag = arms[i].tag;
        const fs::path appdata = work_dir / ("appdata-" + tag);
        // ★ 每臂**全新私有 appdata**（先删干净再建）：复用上一次的 `SAVE.DAT` / `SYS4REG.INI` 会让
        //   游戏读到旧存档，`tail_insns` / 存档大小都会变（实测：脏目录 143,589,634 / 48,016 B vs
        //   干净目录 131,104,827 / 48,128 B）⇒ 读数不可复现。⛔ 只删日志是不够的。
        fs::remove_all(appdata, ec);
        fs::create_directories(appdata, ec);
        if (ec) LIFT_SKIP("清不掉旧的私有 appdata（工作目录被占用？）");
        const fs::path log_path = work_dir / ("e2e-" + tag + ".log");
        fs::remove(log_path, ec);

        // ★ Windows 上**必须**用宽字符拼命令行：`fs::path::string()` 走 ANSI 码页，`天結` 这类
        //   日文路径会被转坏（→ 子进程收不到 `--game-dir`，跑 42k 条就 ExitProcess）。
#if defined(_WIN32)
        std::vector<std::wstring> wargs_v;
        wargs_v.push_back(L"--game-dir");
        wargs_v.push_back(game_dir.wstring());
        wargs_v.push_back(L"--appdata");
        wargs_v.push_back(appdata.wstring());
        wargs_v.push_back(L"--skip-logo");
        wargs_v.push_back(L"--audio");
        wargs_v.push_back(L"off");
        wargs_v.push_back(L"--present");
        wargs_v.push_back(L"headless");
        wargs_v.push_back(L"--raster");
        wargs_v.push_back(L"gpu");
        wargs_v.push_back(L"--input-script");
        wargs_v.push_back(script.wstring());
        wargs_v.push_back(L"--input-stall-boot-ms");
        wargs_v.push_back(L"600000");
        // ★ `--profile` 两臂**都有**（不是差异项）：它只让窗口表可读，不改 guest 指令流
        //   （tests/profile_window.cpp 的 profile-instrumentation-invariant）。
        wargs_v.push_back(L"--profile");
        wargs_v.push_back(L"--profile-top");
        wargs_v.push_back(L"200");
        wargs_v.push_back(L"--time-limit");
        wargs_v.push_back(L"300");
        if (arms[i].with_override) {
            wargs_v.push_back(L"--native-overrides");
            wargs_v.push_back(L"sub_4367B0");
        }
        std::wstring wargs;
        for (const std::wstring& a : wargs_v) wargs += L" " + quoteArg(a);
        const ChildResult r = runChild(exe, wargs, log_path);
#else
        std::vector<std::string> child;
        child.push_back("--game-dir"), child.push_back(game_dir.string());
        child.push_back("--appdata"), child.push_back(appdata.string());
        child.push_back("--skip-logo");
        child.push_back("--audio"), child.push_back("off");
        child.push_back("--present"), child.push_back("headless");
        child.push_back("--raster"), child.push_back("gpu");
        child.push_back("--input-script"), child.push_back(script.string());
        child.push_back("--input-stall-boot-ms"), child.push_back("600000");
        child.push_back("--profile"), child.push_back("--profile-top"), child.push_back("200");
        child.push_back("--time-limit"), child.push_back("300");
        if (arms[i].with_override) {
            child.push_back("--native-overrides"), child.push_back("sub_4367B0");
        }
        const ChildResult r = runChild(exe, child, log_path);
#endif
        if (!r.started) LIFT_SKIP("起不了 emulator-lift 子进程（exe 路径 / 沙箱限制？）");
        logs[i] = readAllBytes(log_path);
        if (logs[i].empty()) LIFT_SKIP("子进程没写日志（工作目录 / 权限问题）");
        std::printf("native-override-e2e: run=%s exit=%d log=%s (%zu 字节)\n", tag.c_str(), r.exit_code,
                    log_path.string().c_str(), logs[i].size());
        if (r.exit_code != 0 && environmentUnavailable(logs[i])) {
            LIFT_SKIP("GPU / 执行源不可用 ⇒ 这条不是判据红");
        }
        static std::string case_name;
        case_name = std::string("e2e/") + tag;
        g_current_case = case_name.c_str();
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(r.exit_code), 0u);  // ①
        LIFT_CHECK(contains(logs[i], "[input-script] finished"));
        tails[i] = parseTail(logs[i]);
        LIFT_CHECK_EQ_I64(tails[i].count, 1);  // 恰好 1 行
        LIFT_CHECK(tails[i].parsed);
        // ④ 停因：必须是引擎自己 ExitProcess，⛔ 不是 time-limit / max-frames
        const std::string stop_line = lineStartingWith(logs[i], "stop      : ");
        LIFT_CHECK(contains(stop_line, "stopped"));
        LIFT_CHECK(contains(lineStartingWith(logs[i], "detail    : "), "ExitProcess"));
        std::printf("native-override-e2e: run=%s tail_insns=%llu tail_wall_ms=%.1f\n", tag.c_str(),
                    (unsigned long long)tails[i].tail_insns, tails[i].tail_wall_ms);
        // ② 台账：`b` 里必须有 `sub_4367B0` 的明细行且 hits==1 / fallbacks==0；`a*` 里必须没有。
        const std::string ovr_line = lineContaining2(logs[i], "sub_4367B0", "hits=");
        if (arms[i].with_override) {
            LIFT_CHECK(!ovr_line.empty());
            std::uint64_t hits = 0;
            std::uint64_t fallbacks = 1;
            LIFT_CHECK(readU64(ovr_line, "hits", &hits));
            LIFT_CHECK(readU64(ovr_line, "fallbacks", &fallbacks));
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(hits), 1u);
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(fallbacks), 0u);
            std::printf("native-override-e2e: run=%s %s\n", tag.c_str(), ovr_line.c_str());
            const std::string prof = lineContaining2(logs[i], "sub_4367B0", "self=");
            if (!prof.empty())
                std::printf("native-override-e2e: run=%s 窗口 profile: %s\n", tag.c_str(), prof.c_str());
        } else {
            LIFT_CHECK(countLinesStartingWith(logs[i], "native-ovr") == 0);
        }
        // 找存档（每个 appdata 是全新的私有目录）
        const fs::path sf = findFile(appdata, "SAVE.DAT");
        const fs::path rf = findFile(appdata, "RT.DAT");
        save_ok[i] = !sf.empty() && readBytesFile(sf, &save[i]);
        rt_ok[i] = !rf.empty() && readBytesFile(rf, &rt[i]);
        LIFT_CHECK(save_ok[i]);  // 存档真的写出来了（否则下面全在"比两个空数组"）
        if (save_ok[i]) {
            std::printf("native-override-e2e: run=%s SAVE.DAT=%zu B\n", tag.c_str(), save[i].size());
        }
        if (g_failures > 0) break;
    }

    // ③ 尾段指令数：`b` 必须显著更少（override 真的替掉了那棵 1.3e8 条的子树）
    if (tails[0].parsed && tails[1].parsed) {
        g_current_case = "e2e/tail-insns";
        std::printf("native-override-e2e: tail_insns a1=%llu b=%llu\n", (unsigned long long)tails[0].tail_insns,
                    (unsigned long long)tails[1].tail_insns);
        LIFT_CHECK(tails[1].tail_insns * 2u < tails[0].tail_insns);
    }
    // ⑤ 墙钟只比更低（⛔ 无绝对阈值：同配置两次 run 实测能差 1.48×）
    if (tails[0].parsed && tails[1].parsed) {
        g_current_case = "e2e/tail-wall";
        std::printf("native-override-e2e: tail_wall_ms a1=%.1f b=%.1f\n", tails[0].tail_wall_ms,
                    tails[1].tail_wall_ms);
        LIFT_CHECK(tails[1].tail_wall_ms < tails[0].tail_wall_ms);
    }
    // ⑥ 存档差分
    if (save_ok[0] && save_ok[1] && save_ok[2]) {
        g_current_case = "e2e/save-reproducible";
        const ByteDiff same = diffBytes(save[0], save[2]);
        LIFT_CHECK(same.same_size);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(same.count), 0);  // 同一臂两次：逐字节相同
        g_current_case = "e2e/save-clock-mask";
        const ByteDiff d = diffBytes(save[0], save[1]);
        LIFT_CHECK(d.same_size);
        std::printf("native-override-e2e: SAVE.DAT a1-vs-b 差异：%zu 字节，区间 [%zu..%zu]；"
                    "掩码 [%u..%u)\n",
                    d.count, d.first, d.last, kSaveClockOffset, kSaveClockOffset + kSaveClockSpan);
        LIFT_CHECK(d.count > 0);  // 时钟真的差（否则"两臂一样"本身就可疑）
        LIFT_CHECK(d.first >= kSaveClockOffset);
        LIFT_CHECK(d.last < kSaveClockOffset + kSaveClockSpan);
    }
    if (rt_ok[0] && rt_ok[2]) {
        g_current_case = "e2e/rt-reproducible";
        const ByteDiff same = diffBytes(rt[0], rt[2]);
        LIFT_CHECK(same.same_size);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(same.count), 0);
    }

    return finish("native-override-e2e");
}
