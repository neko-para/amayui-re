// emulator-lift · **feeder 臂**的段级 override 读数守卫（`bov= / bovd= / bovf=`）
//
// REQ-01M75W6BB3PY3MF42ZTS23KTBS（feeder 也能吃段级 override + PCM 段 `0x48D767`）。
//
// ★ 为什么这条必须在**真 feeder 臂**上跑：`thread_mode_ = (Sdl && realTime)`（`audio.cpp`）⇒
//   默认"真设备 + 窗口"才是 feeder；`--audio virtual/off` 走**主线程 dispatcher**，段级**本来**
//   就能生效 ⇒ 在那一臂上"绿"什么也证明不了（`docs/audio-implementation.md` 的"假绿陷阱 1"）。
// ★ feeder 臂**只能**看读数、不能做 WAV 逐字节（墙钟 + 设备游标）⇒ 本守卫只断言**关系**：
//   ① 子进程 exit 0；
//   ② 日志里同时出现 `feeder=host-thread` 与 `device=ok`（**缺一条 ⇒ SKIP 77**：那不是判据红，
//      是"量错了臂 / 这台机器没有真音频设备"；⛔ 不许把它当成"bov 为 0 也绿"）；
//   ③ 至少一行 fill 的 `bov > 0`（= 段真的进了 feeder 的**私有**段级表并被接管；0 = 没生效）；
//   ④ **所有** fill 行 `bovd == 0` 且 `bovf == 0`（一次 `Declined` 就永久停用 ⇒ 形状没对上）；
//   ⑤ 出现过 `bov > 0` 之后，不允许再出现 `bov == 0` 的行（抓"表没拷进去 / 第一刀之后停用"）。
// ⛔ 不断言 `bov == fills*ch`（那锁"每刀每通道一次"的实现细节；`bov` 是**累计**值，实测**每刀约 48–55**
//    （ch=2，随 ch / 每 quarter 的解码包数变）⇒ ⛔ 不是 `ch`/刀，也⛔ 不是每样本一次）；
// ⛔ 不断言 `ov` 上升/下降：PCM 段会把 `__ftol2_sse` 的**函数级**命中吃掉 ⇒ `ov=` 反而**下降**是
//   正常的（"假绿陷阱 2"）；段级只看 `bov=`，函数级只看 `ov=`。`guest=` 只打印、不进断言。
// ★ 语义证据（"段真的算对了"）在 `tests/native_override_assets.cpp` 的 `pcm/*` 真字节两臂；
//   本守卫只回答"feeder 里到底有没有接管"。
// ★ 子进程输出走**文件重定向**（⛔ 不走管道：受限沙箱里捕获管道会 EPERM）。
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
#include "subprocess_skip.h"

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

struct ChildResult {
    int exit_code = -1;
    bool started = false;
};

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
    // CTest 的 TIMEOUT 是 900 s；这里给 600 s（卡住必须红，⛔ 不许挂死流水线）。
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
    if (rc == -1) return r;
    if (!fs::exists(log_path)) return r;
    r.started = true;
    r.exit_code = WIFEXITED(rc) ? WEXITSTATUS(rc) : (WIFSIGNALED(rc) ? 128 + WTERMSIG(rc) : 1);
    return r;
}
#endif

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool contains(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

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

struct FillLine {
    std::uint64_t bov = 0;
    std::uint64_t bovd = 0;
    std::uint64_t bovf = 0;
    std::uint64_t ov = 0;
    double guest_ms = 0.0;
    bool parsed = false;
};

// 一行一条 fill（`LIFT_AUDIO_TRACE=1` 时由 feeder 收尾 / 主线程泵打印）。
std::vector<FillLine> parseFillLines(const std::string& text) {
    std::vector<FillLine> out;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t nl = text.find('\n', pos);
        const std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (line.find("[audio] feeder fill ") != std::string::npos) {
            FillLine f;
            f.parsed = readU64(line, "bov", &f.bov) && readU64(line, "bovd", &f.bovd) &&
                       readU64(line, "bovf", &f.bovf) && readU64(line, "ov", &f.ov) &&
                       readDouble(line, "guest", &f.guest_ms);
            out.push_back(f);
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return out;
}

// 这台机器 / 这次配置到底有没有"真 feeder 臂"：缺任一条 ⇒ SKIP 77（不是判据红）。
bool feederArmUsable(const std::string& log) {
    if (!contains(log, "feeder=host-thread")) return false;
    if (!contains(log, "device=ok")) return false;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr, "用法：%s <emulator-lift> <game-dir> <work-dir> <interaction-graph>\n", argv[0]);
        return 2;
    }
    const fs::path exe = argv[1];
    const std::string game_dir = argv[2];
    const fs::path work = argv[3];
    const std::string graph = argv[4];
    std::error_code ec;
    fs::create_directories(work, ec);
    if (!fs::exists(exe)) LIFT_SKIP("没有 emulator-lift 可执行（构建目录不对）");
    if (!fs::exists(graph)) LIFT_SKIP("没有 interaction-graph（不在源码树里）");
    if (!fs::exists(fs::path(game_dir) / "SYS4INI.BIN")) LIFT_SKIP("没有游戏安装树（SYS4INI.BIN）");

    // 真 feeder = **真设备 + 宿主时钟** ⇒ 必须 `--audio sdl` 且 `--present window`（后者隐含 --real-time）。
    // `--verbose`（= log-level info）是为了让 `feeder=host-thread` 那行**真的出现**：缺省 `warn` 会把它滤掉，
    // 于是"有没有量错臂"在日志里看不出来（那正是本守卫要排除的假绿陷阱 1）。
    // ★ GPU 是唯一光栅实现：没显卡的机器由 `environmentUnavailable` 判成 SKIP（⛔ 不退回 CPU）。
    const fs::path log_path = work / "feeder-seg.log";
    const fs::path appdata = work / "appdata";
    fs::create_directories(appdata, ec);
    const std::vector<std::string> args = {
        "--verbose",
        "--appdata", appdata.string(),
        "--game-dir", game_dir,
        "--present", "window",
        "--present-hidden",
        "--native-overrides", "all",
        "--skip-logo",
        "--audio", "sdl",
        "--interaction-graph", graph,
    };
#if defined(_WIN32)
    std::wstring wide_args;
    for (const std::string& a : args) {
        if (!wide_args.empty()) wide_args += L" ";
        wide_args += quoteArg(std::wstring(a.begin(), a.end()));
    }
    _putenv_s("LIFT_AUDIO_TRACE", "1");
    const ChildResult child = runChild(exe, wide_args, log_path);
#else
    setenv("LIFT_AUDIO_TRACE", "1", 1);
    const ChildResult child = runChild(exe, args, log_path);
#endif
    if (!child.started) LIFT_SKIP("子进程没起来（环境不允许 spawn）");
    const std::string log = readAll(log_path);

    // ★ 无 GPU（含 `SDL_VIDEODRIVER=dummy`）：子进程在跑 guest 之前就拒绝启动 ⇒ SKIP 77。
    //   （下面 `feederArmUsable` 也会因缺 `device=ok` 而 SKIP，但这条把原因说得更准。）
    LIFT_SKIP_IF_NO_GPU(child.exit_code, log);

    // ② 假绿陷阱 1 的机械版：先确认"这确实是真 feeder 臂"。
    if (!feederArmUsable(log)) {
        std::printf("SKIP: 这次跑的不是真 feeder 臂（缺 `feeder=host-thread` 或 `device=ok`）\n");
        std::printf("      —— 真音频设备不可用 / 窗口没起来都会落这里；⛔ 这不是 `bov` 判据红\n");
        return 77;
    }
    LIFT_CHECK(child.exit_code == 0);
    const std::vector<FillLine> fills = parseFillLines(log);
    LIFT_CHECK(!fills.empty());
    if (fills.empty()) {
        std::printf("  日志里没有任何 `[audio] feeder fill` 行（LIFT_AUDIO_TRACE 没生效？）\n");
        return lift_test::finish("feeder-seg");
    }
    bool any_parsed = true;
    for (const FillLine& f : fills) any_parsed = any_parsed && f.parsed;
    LIFT_CHECK(any_parsed);
    // ③ 至少一行真的接管过。
    bool any_positive = false;
    for (const FillLine& f : fills) any_positive = any_positive || f.bov > 0;
    LIFT_CHECK(any_positive);
    // ④ 所有行都没有 Declined / Failed（一次 Declined 就永久停用 ⇒ 形状没对上 = 硬错）。
    for (std::size_t i = 0; i < fills.size(); ++i) {
        if (fills[i].bovd != 0 || fills[i].bovf != 0) {
            lift_test::failf(__FILE__, __LINE__,
                             "第 %zu 条 fill：bovd=%llu bovf=%llu（段级在 feeder 里回退了）", i,
                             (unsigned long long)fills[i].bovd, (unsigned long long)fills[i].bovf);
        }
    }
    // ⑤ 出现过 >0 之后不允许再出现 0（抓"表没拷进去 / 第一刀之后停用"）。
    bool seen_positive = false;
    for (std::size_t i = 0; i < fills.size(); ++i) {
        if (fills[i].bov > 0) seen_positive = true;
        else if (seen_positive)
            lift_test::failf(__FILE__, __LINE__, "第 %zu 条 fill：bov 回退到 0（前面已经 >0）", i);
    }
    // 读数（⛔ 不是断言）：首/末条的 bov / ov / guest，以及 bov 的增长率。
    std::printf("feeder-seg: fills=%zu 首条 bov=%llu ov=%llu guest=%.2f ms  末条 bov=%llu ov=%llu guest=%.2f ms\n",
                fills.size(), (unsigned long long)fills.front().bov, (unsigned long long)fills.front().ov,
                fills.front().guest_ms, (unsigned long long)fills.back().bov, (unsigned long long)fills.back().ov,
                fills.back().guest_ms);
    return lift_test::finish("feeder-seg");
}
