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
// ★ **必须在 RelWithDebInfo 树里跑**（2026-11-07 起 `windows-x64` preset 就是 RelWithDebInfo；
//   解释器慢到 boot 到影片就要几分钟，`--time-limit` 会把影片截断 ⇒ 这条会假红。测试用
//   没有 Debug preset）。测试用 `$<TARGET_FILE:emulator-lift>`，跟着所在构建树走。
//   ★ Debug 树里本测试**自己 `LIFT_SKIP`(77)**（`LIFT_MOVIE_LOGO_IS_DEBUG`）—— 不跑，免得占 5 分钟。
// ★ 缺游戏安装 ⇒ CMake 侧不注册；GPU 不可用（raster 行不是 sdl-gpu）⇒ `LIFT_SKIP`(77) 并打印原因。
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "check.h"
#include "runtime/host_encoding.h"

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
    //   而这条测试要 boot 到 LOGO 再放完 8.6 s 影片 —— 在 Debug 里会撞 `--time-limit` 假红/假绿。
    //   canonical：`ctest --test-dir out-rel -R movie-logo`（预设 `windows-x64-rel`）。
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
    for (const wchar_t* f : {L"--raster", L"gpu"}) add(f);
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

    // GPU 不可用 ⇒ 这条测试不是"影片红"，是"环境红" ⇒ SKIP 77 并打印原因。
    const std::size_t raster_pos = log.find("raster    :");
    if (raster_pos != std::string::npos) {
        const std::string raster_line = log.substr(raster_pos, 120);
        if (raster_line.find("sdl-gpu") == std::string::npos) {
            std::printf("SKIP: raster 不是 sdl-gpu ⇒ %s\n", raster_line.c_str());
            return 77;
        }
    }

    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(r.exit_code), 0u);
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

    return lift_test::finish("movie-logo");
#endif
}
