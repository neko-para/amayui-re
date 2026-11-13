// emulator-lift · **assets 档**守卫：真机安装上的 LOGO 影片必须"真的放完"
//
// 判据（红了意味着什么）：
//   ① 子进程退出码 0（不是 guest fault / decode-error / unimplemented）；
//   ② 日志里出现 `首帧已送渲染器`（不是"接线成功但一帧没送"）；
//   ③ `samples=` ≥ 20（`--real-time` 下会丢帧；"播完"由 `completed=true` 守）；
//   ④ `HeapAlloc 大块` 次数 ≤ 4 —— `IMediaSample2::GetProperties` 的 `pMediaType` 一旦回退成非 0，
//      `PrepareReceive` 就会每帧 ApplyMediaType ⇒ 渲染器每帧 `operator new[](w*h)` 不释放 ⇒ 这条必红；
//   ⑤ run summary 的 `heap : in_use` < 64 MB（修前是 132 MB 且耗尽）；
//   ⑥ （非判据）`渲染器服务线程事件` 是兜底路径的痕迹：渲染器 Running 时不需要它；
//   ⑦ **像素**：`[dshow] tex movie #` 行里出现 1280×720、`nonzero > 0`、且至少两个不同 `hash`
//      （引擎 `DoRenderSample` 写进影片纹理的像素真的在变），并且这些行在 `首帧已送渲染器` 之后。
//
// ★ 子进程输出走**文件重定向**（不走管道：受限沙箱里捕获管道会 EPERM）。
// ★ 命令行**不含 `--skip-logo`**（本测试专门守"影片真的跑"，跳过模式会让它失去意义）。
// ★ **必须在 RelWithDebInfo 树里跑**（`windows-x64` preset 的缺省就是它；Debug 下解释器慢到
//   boot 到影片就要几分钟，`--time-limit` 会把影片截断 ⇒ 这条会假红）。测试用
//   `$<TARGET_FILE:emulator-lift>`，跟着所在构建树走。
//   ★ Debug 树里本测试**自己 `LIFT_SKIP`(77)**（`LIFT_MOVIE_LOGO_IS_DEBUG`）—— 不跑，免得白等。
// ★ CMake 侧是 **opt-in**：默认不注册，`LIFT_ASSETS=1` 才注册（`add_executable` 也在同一个 `if`
//   里）。CTest 给它注入 `LIFT_STOP_AFTER_MOVIE=1`（影片放完即停，不再空转到 `--time-limit`），
//   并传第 5 个参数 `--expect-early-stop`；本测试据此**断言钩子真的生效**（判据不挂在环境变量上：
//   否则钩子失效时只会变慢、仍然绿）。
// ★ 缺游戏安装 ⇒ CMake 侧不注册；GPU 不可用（raster 行不是 sdl-gpu）⇒ `LIFT_SKIP`(77) 并打印原因。
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "check.h"
#include "runtime/host_encoding.h"
#include "subprocess_skip.h"

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

namespace fs = std::filesystem;

std::string readAllBytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::wstring quoteArg(const std::wstring& s) {
    // 路径不含引号；包一层双引号即可（本测试自己控制参数，不做通用 quoting）。
    return L"\"" + s + L"\"";
}

#if defined(_WIN32)
struct ChildResult {
    DWORD exit_code = 0xFFFFFFFFu;
    bool started = false;
};

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
    // 10 分钟上限（正常 ~90 s；卡住时必须红，而不是挂死流水线）。
    const DWORD waited = ::WaitForSingleObject(pi.hProcess, 600000u);
    if (waited != WAIT_OBJECT_0) {
        ::TerminateProcess(pi.hProcess, 991u);
        ::WaitForSingleObject(pi.hProcess, 10000u);
    }
    ::GetExitCodeProcess(pi.hProcess, &r.exit_code);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return r;
}
#endif

// 最后一个 `samples=NNN` 的 NNN（没有 ⇒ -1）。
long lastSamples(const std::string& log) {
    long best = -1;
    std::size_t pos = 0;
    const std::string key = "samples=";
    while ((pos = log.find(key, pos)) != std::string::npos) {
        pos += key.size();
        long v = 0;
        bool any = false;
        while (pos < log.size() && log[pos] >= '0' && log[pos] <= '9') {
            v = v * 10 + (log[pos] - '0');
            ++pos;
            any = true;
        }
        if (any && v > best) best = v;
    }
    return best;
}

int countOccurrences(const std::string& hay, const std::string& needle) {
    int n = 0;
    std::size_t pos = 0;
    while ((pos = hay.find(needle, pos)) != std::string::npos) {
        ++n;
        pos += needle.size();
    }
    return n;
}

// 从 run summary 的 `heap      : in_use N peak ...` 取 N（MB）。找不到 ⇒ -1。
long heapInUseMb(const std::string& log) {
    const std::string key = "in_use ";
    const std::size_t pos = log.find(key);
    if (pos == std::string::npos) return -1;
    std::size_t i = pos + key.size();
    long v = 0;
    bool any = false;
    while (i < log.size() && log[i] >= '0' && log[i] <= '9') {
        v = v * 10 + (log[i] - '0');
        ++i;
        any = true;
    }
    return any ? v / (1024 * 1024) : -1;
}

}  // namespace

int main(int argc, char** argv) {
#if !defined(_WIN32)
    (void)argc;
    (void)argv;
    LIFT_SKIP("movie-logo 只在 Windows 宿主上跑（CreateProcessW + 真机安装）");
#else
    // ★ Debug 树里**不跑**（`LIFT_SKIP` 77）：用户口径是"Debug 解释器慢到无法使用"，
    //   而这条测试要 boot 到 LOGO 再放完影片 —— 在 Debug 里会撞 `--time-limit` 假红/假绿。
    //   canonical：默认 preset `windows-x64`（RelWithDebInfo）的 `out/`。
#if defined(LIFT_MOVIE_LOGO_IS_DEBUG) && LIFT_MOVIE_LOGO_IS_DEBUG
    LIFT_SKIP("movie-logo 只在 RelWithDebInfo 树里跑（Debug 解释器太慢；用默认 preset）");
#endif
    const std::vector<std::string> args = lift::argvUtf8(argc, argv);
    if (args.size() < 5) {
        LIFT_SKIP("需要 CTest 传 <emulator-lift.exe> <corpus.exe> <game-dir> <appdata-dir>");
    }
    const fs::path exe = fs::path(lift::utf8ToWide(args[1]));
    const fs::path corpus = fs::path(lift::utf8ToWide(args[2]));
    const fs::path game_dir = fs::path(lift::utf8ToWide(args[3]));
    const fs::path work_dir = fs::path(lift::utf8ToWide(args[4]));
    // 第 5 个参数（可选）= `--expect-early-stop`：与 CTest 注入的 `LIFT_STOP_AFTER_MOVIE=1` 成对。
    const bool expect_early_stop = args.size() > 5 && args[5] == "--expect-early-stop";
    std::error_code ec;
    fs::create_directories(work_dir, ec);
    const fs::path log_path = work_dir / "movie-logo.log";
    const fs::path appdata = work_dir / "appdata";

    // ---- 固定参数（⛔ 绝不含 --skip-logo）----
    std::wstring cmd;
    auto add = [&cmd](const wchar_t* flag) {
        if (!cmd.empty()) cmd += L" ";
        cmd += flag;
    };
    add(L"--exe");
    cmd += L" " + quoteArg(corpus.wstring());
    add(L"--game-dir");
    cmd += L" " + quoteArg(game_dir.wstring());
    add(L"--appdata");
    cmd += L" " + quoteArg(appdata.wstring());
    for (const wchar_t* f : {L"--present", L"headless"}) add(f);
    for (const wchar_t* f : {L"--real-time"}) add(f);
    for (const wchar_t* f : {L"--native-overrides", L"all"}) add(f);
    for (const wchar_t* f : {L"--max-insns", L"4000000000"}) add(f);
    for (const wchar_t* f : {L"--time-limit", L"45"}) add(f);
    for (const wchar_t* f : {L"--log-level", L"info"}) add(f);
    // 像素判据走 `video` 分类（`[dshow] tex movie #`）；`d3d9`/`shim` 是排障用的额外分类。
    for (const wchar_t* f : {L"--log-cat", L"video,shim"}) add(f);
    for (const wchar_t* f : {L"--verbose"}) add(f);

    const std::string narrow_cmd(args.size() > 0 ? args[0] : "");
    (void)narrow_cmd;
    LIFT_CHECK(cmd.find(L"--skip-logo") == std::wstring::npos);

    const ChildResult r = runChild(exe, cmd, log_path);
    if (!r.started) {
        LIFT_SKIP("起不了 emulator-lift 子进程（exe 路径不对？）");
    }
    const std::string log = readAllBytes(log_path);
    std::fprintf(stdout, "movie-logo: exit=%lu log=%s (%zu 字节)\n", static_cast<unsigned long>(r.exit_code),
                 log_path.string().c_str(), log.size());
    if (log.empty()) {
        LIFT_SKIP("子进程没写日志（工作目录 / 权限问题）");
    }

    // ★ 无 GPU：子进程在跑 guest 之前就拒绝启动 ⇒ 这不是"影片红"，是环境红 ⇒ SKIP 77。
    //   （先判这条：拒绝启动时日志里**没有** raster 行，旧逻辑会漏到 exit==0 断言上变红。）
    LIFT_SKIP_IF_NO_GPU(r.exit_code, log);

    // GPU 不可用（另外的形态：起了步但 raster 不是 sdl-gpu）⇒ SKIP 77 并打印原因。
    const std::size_t raster_pos = log.find("raster    :");
    if (raster_pos != std::string::npos) {
        const std::string raster_line = log.substr(raster_pos, 120);
        if (raster_line.find("sdl-gpu") == std::string::npos) {
            std::printf("SKIP: raster 不是 sdl-gpu ⇒ %s\n", raster_line.c_str());
            return 77;
        }
    }

    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(r.exit_code), 0u);
    // ★ 钩子自检（`--expect-early-stop`）：影片放完即停 ⇒ run summary 的停止原因是 `stopped`
    //   而不是 `time-limit`。没有这一条，钩子失效时本测试只会"变慢但仍然绿"—— 那正是本任务要治的白等。
    if (expect_early_stop) {
        LIFT_CHECK(log.find("stop      : stopped") != std::string::npos);
    }
    LIFT_CHECK(log.find("首帧已送渲染器") != std::string::npos);
    const long samples = lastSamples(log);
    // 帧数：`--real-time` 下引擎按墙钟走，渲染慢就**丢帧**（与真机一致）⇒ 只要求"确实在推进"，
    //   不要求 259 帧；"播完"由 `completed=true` 守。
    LIFT_CHECK(samples >= 20);
    LIFT_CHECK(log.find("completed=true") != std::string::npos);
    const int big_allocs = countOccurrences(log, "HeapAlloc 大块");
    std::fprintf(stdout, "movie-logo: samples=%ld big_allocs=%d heap_in_use_mb=%ld\n", samples, big_allocs,
                 heapInUseMb(log));
    LIFT_CHECK(big_allocs <= 4);
    const long in_use_mb = heapInUseMb(log);
    if (in_use_mb < 0) {
        std::printf("movie-logo: 解析不到 `heap : in_use` ⇒ 跳过这条断言\n");
    } else {
        LIFT_CHECK(in_use_mb < 64);
    }
    // ⑥ 服务线程事件的宿主合成是**兜底**：渲染器进入 Running 后引擎自己按流时间渲染，
    //   不再需要合成那条等待 ⇒ 只在日志里出现时记录，不作为判据。
    if (log.find("渲染器服务线程事件") != std::string::npos) {
        std::printf("movie-logo: 渲染器服务线程事件由宿主合成（兜底路径被走到）\n");
    }

    // ---- ⑦ 像素判据（doc §8.13）：引擎 `DoRenderSample` 写进**影片纹理**的那张必须真有像素、帧间在变。
    //   `[dshow] tex movie #n w=1280 h=720 nonzero=… sampled=… hash=…`
    //   注意：引擎对影片纹理只在开始时 Lock 一次（之后用缓存的 pBits 写），所以观察点在 Present。
    long tex_movie_lines = 0;
    long tex_movie_nonzero_max = 0;
    bool tex_movie_size_ok = false;
    bool tex_movie_after_first_frame = true;
    std::size_t distinct_hashes = 0;
    {
        std::vector<std::string> seen_hashes;
        const std::size_t first_frame = log.find("首帧已送渲染器");
        std::size_t at = 0;
        while ((at = log.find("tex movie #", at)) != std::string::npos) {
            const std::size_t eol = log.find('\n', at);
            const std::string line = log.substr(at, eol == std::string::npos ? std::string::npos : eol - at);
            unsigned n = 0, w = 0, h = 0, nz = 0, sampled = 0;
            unsigned long long hash = 0;
            if (std::sscanf(line.c_str(), "tex movie #%u w=%u h=%u nonzero=%u sampled=%u hash=%llx", &n, &w, &h,
                            &nz, &sampled, &hash) == 6) {
                ++tex_movie_lines;
                if (w == 1280u && h == 720u) tex_movie_size_ok = true;
                if (static_cast<long>(nz) > tex_movie_nonzero_max) tex_movie_nonzero_max = static_cast<long>(nz);
                char hex[32];
                std::snprintf(hex, sizeof(hex), "%016llx", hash);
                bool dup = false;
                for (const auto& s : seen_hashes) dup = dup || (s == hex);
                if (!dup) seen_hashes.emplace_back(hex);
                if (first_frame != std::string::npos && at < first_frame) tex_movie_after_first_frame = false;
            }
            at = (eol == std::string::npos) ? log.size() : eol;
        }
        distinct_hashes = seen_hashes.size();
    }
    std::fprintf(stdout, "movie-logo: tex_movie_lines=%ld nonzero_max=%ld distinct_hashes=%zu\n", tex_movie_lines,
                 tex_movie_nonzero_max, distinct_hashes);
    LIFT_CHECK(tex_movie_lines >= 2);          // 至少两帧进了纹理
    LIFT_CHECK(tex_movie_size_ok);             // 是 1280×720 那张影片纹理
    LIFT_CHECK(tex_movie_nonzero_max > 0);     // 纹理里真的出现了非黑像素
    LIFT_CHECK(distinct_hashes >= 2);          // 帧间内容在变（不是一张静止图）
    LIFT_CHECK(tex_movie_after_first_frame);   // 像素出现在"首帧已送渲染器"之后 ⇒ 与影片时间绑定

    // ---- ⑧ native override 差分（doc §8.15）：开/关 `sub_487C60`，第一帧纹理必须**逐字节一致** ----
    //   只比 `tex movie #1`（pts=0 的第一帧内容确定）；同时用 `samples` 证明 override 真的把影片
    //   拉回了 30fps（解释器路径在 `--real-time` 下会大量丢帧）。
    std::wstring cmd_b = cmd;
    const std::wstring nv_a = L"--native-overrides all";
    const std::wstring nv_b = L"--native-overrides all,-sub_487C60";
    const std::size_t nv_pos = cmd_b.find(nv_a);
    LIFT_CHECK(nv_pos != std::wstring::npos);
    if (nv_pos != std::wstring::npos) cmd_b.replace(nv_pos, nv_a.size(), nv_b);
    // ★ 第二次跑用**独立的 appdata**：引擎可能把"看过 LOGO"写进配置，复用会直接跳过影片。
    const fs::path appdata_b = work_dir / "appdata-b";
    fs::create_directories(appdata_b, ec);
    const std::wstring appdata_a_arg = quoteArg(appdata.wstring());
    const std::size_t ad_pos = cmd_b.find(appdata_a_arg);
    LIFT_CHECK(ad_pos != std::wstring::npos);
    if (ad_pos != std::wstring::npos) cmd_b.replace(ad_pos, appdata_a_arg.size(), quoteArg(appdata_b.wstring()));
    const fs::path log_b = work_dir / "movie-logo-nooverride.log";
    const ChildResult rb = runChild(exe, cmd_b, log_b);
    const std::string logb = readAllBytes(log_b);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(rb.exit_code), 0u);
    // ★ 第二个子进程走**同一条**停机链 ⇒ 也必须提前停（否则整条测试只慢一半、照样绿）。
    if (expect_early_stop) {
        LIFT_CHECK(logb.find("stop      : stopped") != std::string::npos);
    }
    auto texMovie1 = [](const std::string& text) -> std::string {
        const std::size_t at = text.find("tex movie #1 ");
        if (at == std::string::npos) return {};
        const std::size_t eol = text.find('\n', at);
        return text.substr(at + 13, (eol == std::string::npos ? text.size() : eol) - at - 13);
    };
    const std::string t1_a = texMovie1(log);
    const std::string t1_b = texMovie1(logb);
    std::printf("movie-logo: tex#1 override=[%s]\n", t1_a.c_str());
    std::printf("movie-logo: tex#1 interpret=[%s]\n", t1_b.c_str());
    LIFT_CHECK(!t1_a.empty() && t1_a == t1_b);  // 逐字节一致（w/h/nonzero/sampled/hash 全同）
    const long samples_b = lastSamples(logb);
    std::printf("movie-logo: samples override=%ld interpret=%ld\n", samples, samples_b);
    LIFT_CHECK(samples > samples_b);  // 覆盖后跟得上 30fps（解释器路径会丢帧）

    // ---- ⑨ 影片音频（doc §8.18）：必须真的解码并推给设备（同步在 dshow::AudioRenderer）----
    const std::size_t audio_at = log.find("音频渲染器：Stop");
    long audio_fed = -1;
    if (audio_at != std::string::npos) {
        const std::size_t fed_at = log.find("fed=", audio_at);
        if (fed_at != std::string::npos) audio_fed = std::strtol(log.c_str() + fed_at + 4, nullptr, 10);
    }
    std::printf("movie-logo: audio_fed_frames=%ld\n", audio_fed);
    // 8.642 s × 44100 ≈ 381k 帧；至少 4.5 s 的解码与推送才算"音频真的接上了"。
    LIFT_CHECK(audio_fed > 200000);

    return lift_test::finish("movie-logo");
#endif
}
