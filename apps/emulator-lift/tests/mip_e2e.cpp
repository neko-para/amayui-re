// emulator-lift · **assets 档（可 SKIP）**端到端守卫：派生 mip 链真的在 TITLE 帧上被选中
// （`REQ-01M73G2724479XAVS176WBZ65Q`）
//
// 形制照 `tests/native_override_e2e.cpp`：同一份 repro、同一份二进制，**唯一差异**是
// `LIFT_NO_MIP=1`（诊断开关：把 mip 选级关掉 ⇒ 恒 level 0）。两臂各跑一次 `--max-frames 2`。
//
// 判据（红了意味着什么；缺一条都不算）：
//   ① 两臂都 exit 0、都导出了 `frame_0002.bmp`；
//   ② **开臂**日志里至少一条 `[draw]` 的 `lvl=a..b` 有 `b >= 1`，且至少一条 `miprb=` > 0
//      —— 即"真实的缩小批真的取到了派生级、派生链真的被算出来了"（⛔ 不是"只加了代码"）；
//   ③ **关臂**所有 `[draw]` 都是 `lvl=0..0` 且 `miprb=0` —— 对照臂必须真的关掉（否则 ② 可被
//      "恒取高档"冒充）；
//   ④ **不改变已正确的情形**：开臂里 `bbox=0,0..1280,720` 的**全屏背景层**（2048² 的
//      `texserial=`，本 repro 里是放大采样）必须仍是 `lvl=0..0`。
//   ⑤ 读数（⛔ **不**断言具体数字）：右侧区域 `(1060,40) 200×640` 的 11×11 中值法点密度与
//      逐行高频能量，两臂都打印出来 —— 那是"mip 有没有改变这一片"的证据，会随场景变，属读数。
// ★ 环境不可用（没有游戏安装 / 执行源）⇒ `LIFT_SKIP`(77)，那不是判据红。
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

// 与 `.tmp/d3d9/a5-artifact.md` §A5-2 同一块区域 / 同一个判据（11×11 中值，阈值 25）。
constexpr int kRegionX = 1060;
constexpr int kRegionY = 40;
constexpr int kRegionW = 200;
constexpr int kRegionH = 640;
constexpr int kMedianK = 11;
constexpr int kMedianThreshold = 25;

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
    // 每次 run 上限 8 分钟（两臂总和 < CTest 的 TIMEOUT 1200）；卡住必须红，⛔ 不许挂死流水线。
    const DWORD waited = ::WaitForSingleObject(pi.hProcess, 480000u);
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

ChildResult runChild(const fs::path& exe, const std::string& args, const fs::path& log_path) {
    ChildResult r;
    const std::string cmd = shellQuote(exe.string()) + " " + args + " > " + shellQuote(log_path.string()) + " 2>&1";
    const int rc = std::system(cmd.c_str());
    r.started = true;
    r.exit_code = (rc >= 0 && WIFEXITED(rc)) ? WEXITSTATUS(rc) : -1;
    return r;
}
#endif

void setEnvVar(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    if (value == nullptr || *value == '\0') unsetenv(name);
    else setenv(name, value, 1);
#endif
}

// `[draw]` 行里的一个读数（只解析本守卫要的字段）。
struct DrawLine {
    bool valid = false;
    int bbox_x0 = 0, bbox_y0 = 0, bbox_x1 = 0, bbox_y1 = 0;
    std::uint32_t tex_w = 0, tex_h = 0;
    std::uint32_t lvl_min = 0, lvl_max = 0;
    unsigned long long miprb = 0;
};

bool readU32(const std::string& line, const char* key, std::uint32_t* out) {
    const std::string k = std::string(key) + "=";
    const std::size_t at = line.find(k);
    if (at == std::string::npos) return false;
    const char* begin = line.c_str() + at + k.size();
    char* end = nullptr;
    const unsigned long v = std::strtoul(begin, &end, 10);
    if (end == begin) return false;
    *out = static_cast<std::uint32_t>(v);
    return true;
}

bool readULL(const std::string& line, const char* key, unsigned long long* out) {
    const std::string k = std::string(key) + "=";
    const std::size_t at = line.find(k);
    if (at == std::string::npos) return false;
    const char* begin = line.c_str() + at + k.size();
    char* end = nullptr;
    const unsigned long long v = std::strtoull(begin, &end, 10);
    if (end == begin) return false;
    *out = v;
    return true;
}

// `lvl=a..b`
bool readLevelRange(const std::string& line, std::uint32_t* lo, std::uint32_t* hi) {
    const std::size_t at = line.find(" lvl=");
    if (at == std::string::npos) return false;
    const char* begin = line.c_str() + at + 5;
    char* end = nullptr;
    const unsigned long a = std::strtoul(begin, &end, 10);
    if (end == begin || *end != '.') return false;
    ++end;
    if (*end != '.') return false;
    ++end;
    char* end2 = nullptr;
    const unsigned long b = std::strtoul(end, &end2, 10);
    if (end2 == end) return false;
    *lo = static_cast<std::uint32_t>(a);
    *hi = static_cast<std::uint32_t>(b);
    return true;
}

// `bbox=x0,y0..x1,y1`
bool readBBox(const std::string& line, int* x0, int* y0, int* x1, int* y1) {
    const std::size_t at = line.find(" bbox=");
    if (at == std::string::npos) return false;
    const char* p = line.c_str() + at + 6;
    int v[4] = {0, 0, 0, 0};
    for (int i = 0; i < 4; ++i) {
        char* end = nullptr;
        v[i] = static_cast<int>(std::strtol(p, &end, 10));
        if (end == p) return false;
        p = end;
        // 分隔符：x0 后是 ','、y0 后是 ".."、x1 后是 ','
        if (i == 0 || i == 2) {
            if (*p != ',') return false;
            ++p;
        } else if (i == 1) {
            if (p[0] != '.' || p[1] != '.') return false;
            p += 2;
        }
    }
    *x0 = v[0];
    *y0 = v[1];
    *x1 = v[2];
    *y1 = v[3];
    return true;
}

std::vector<DrawLine> parseDrawLines(const std::string& log) {
    std::vector<DrawLine> out;
    std::size_t pos = 0;
    while ((pos = log.find("[draw] ", pos)) != std::string::npos) {
        const std::size_t eol = log.find('\n', pos);
        const std::string line = log.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
        pos = eol == std::string::npos ? log.size() : eol + 1;
        // 只统计 frame=2（本守卫的帧）。
        std::uint32_t frame = 0;
        if (!readU32(line, "frame", &frame) || frame != 2u) continue;
        DrawLine d;
        d.valid = readLevelRange(line, &d.lvl_min, &d.lvl_max) && readBBox(line, &d.bbox_x0, &d.bbox_y0, &d.bbox_x1, &d.bbox_y1);
        if (!d.valid) continue;
        // `2048x2048`（texfmt 段之后）—— 只用来确认全屏层是 2048²
        const std::size_t at = line.find(" texfmt=");
        if (at != std::string::npos) {
            const std::size_t sp = line.find(' ', at + 8);
            if (sp != std::string::npos) {
                const char* p = line.c_str() + sp + 1;
                char* end = nullptr;
                const unsigned long w = std::strtoul(p, &end, 10);
                if (end != p && *end == 'x') {
                    char* end2 = nullptr;
                    const unsigned long h = std::strtoul(end + 1, &end2, 10);
                    if (end2 != end + 1) {
                        d.tex_w = static_cast<std::uint32_t>(w);
                        d.tex_h = static_cast<std::uint32_t>(h);
                    }
                }
            }
        }
        if (!readULL(line, "miprb", &d.miprb)) d.miprb = 0;
        out.push_back(d);
    }
    return out;
}

// ---- 右侧区域读数（11×11 中值法；与 A5-2 的 `dot_sign2.mjs` 同口径）----
struct RegionRead {
    bool valid = false;
    int width = 0, height = 0;
    double density = 0.0;   // |L - median11x11| > 25 的点占区域的比例（%）
    double row_hf = 0.0;    // 逐行相邻像素亮度差的均值
};

RegionRead measureRegion(const fs::path& bmp) {
    RegionRead r;
    const std::string bytes = readAllBytes(bmp);
    if (bytes.size() < 54) return r;
    const auto* b = reinterpret_cast<const std::uint8_t*>(bytes.data());
    if (b[0] != 'B' || b[1] != 'M') return r;
    auto rd32 = [&](std::size_t off) {
        return static_cast<std::uint32_t>(b[off]) | (static_cast<std::uint32_t>(b[off + 1]) << 8) |
               (static_cast<std::uint32_t>(b[off + 2]) << 16) | (static_cast<std::uint32_t>(b[off + 3]) << 24);
    };
    const std::uint32_t data_off = rd32(10);
    const int iw = static_cast<int>(rd32(18));
    const int ih = static_cast<int>(rd32(22));
    const std::uint16_t bpp = static_cast<std::uint16_t>(b[28] | (b[29] << 8));
    if (bpp != 32u || iw <= 0 || ih <= 0) return r;
    if (static_cast<std::size_t>(data_off) >= bytes.size()) return r;
    r.width = iw;
    r.height = ih;
    const std::size_t row = static_cast<std::size_t>(iw) * 4u;
    auto lum = [&](int x, int y) {
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x >= iw) x = iw - 1;
        if (y >= ih) y = ih - 1;
        const std::size_t p = data_off + static_cast<std::size_t>(ih - 1 - y) * row + static_cast<std::size_t>(x) * 4u;
        return 0.114 * b[p] + 0.587 * b[p + 1] + 0.299 * b[p + 2];
    };
    const int x0 = kRegionX < iw ? kRegionX : 0;
    const int y0 = kRegionY < ih ? kRegionY : 0;
    const int w = (x0 + kRegionW <= iw) ? kRegionW : iw - x0;
    const int h = (y0 + kRegionH <= ih) ? kRegionH : ih - y0;
    if (w <= 2 || h <= 2) return r;
    const int half = (kMedianK - 1) / 2;
    std::vector<double> win(static_cast<std::size_t>(kMedianK) * kMedianK);
    long long points = 0, pixels = 0;
    double row_sum = 0.0;
    long long row_pairs = 0;
    for (int y = 0; y < h; ++y) {
        double prev = 0.0;
        bool have_prev = false;
        for (int x = 0; x < w; ++x) {
            std::size_t n = 0;
            for (int dy = -half; dy <= half; ++dy)
                for (int dx = -half; dx <= half; ++dx) win[n++] = lum(x0 + x + dx, y0 + y + dy);
            std::sort(win.begin(), win.end());
            const double med = win[win.size() / 2];
            const double cur = lum(x0 + x, y0 + y);
            if (std::fabs(cur - med) > static_cast<double>(kMedianThreshold)) ++points;
            ++pixels;
            if (have_prev) {
                row_sum += std::fabs(cur - prev);
                ++row_pairs;
            }
            prev = cur;
            have_prev = true;
        }
    }
    r.valid = pixels != 0;
    if (r.valid) {
        r.density = 100.0 * static_cast<double>(points) / static_cast<double>(pixels);
        r.row_hf = row_pairs != 0 ? row_sum / static_cast<double>(row_pairs) : 0.0;
    }
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace lift_test;
    g_current_case = "mip-e2e";

    const std::vector<std::string> args = lift::argvUtf8(argc, argv);
    if (args.size() < 4) {
        std::fprintf(stderr, "用法：lift-mip-e2e <emulator-lift> <game-dir> <work-dir>\n");
        return 2;
    }
    const fs::path exe = fs::path(lift::utf8ToWide(args[1]));
    fs::path game_dir = fs::path(lift::utf8ToWide(args[2]));
    if (const char* env_game = std::getenv("LIFT_ASSETS_GAME_DIR"); env_game != nullptr && *env_game != '\0') {
        game_dir = fs::path(lift::utf8ToWide(env_game));
    }
    const fs::path work_dir = fs::path(lift::utf8ToWide(args[3]));
    LIFT_CHECK(fs::exists(exe));
    LIFT_CHECK(fs::exists(game_dir / "SYS4INI.BIN"));
    if (!fs::exists(exe) || !fs::exists(game_dir / "SYS4INI.BIN")) LIFT_SKIP("缺 exe / 游戏安装");

    std::error_code ec;
    fs::create_directories(work_dir, ec);
    if (ec) LIFT_SKIP("建不了工作目录");

    struct Arm {
        const char* tag;
        bool mip_on;
    };
    const Arm arms[2] = {{"on", true}, {"off", false}};
    std::string logs[2];
    RegionRead reads[2];

    for (int i = 0; i < 2; ++i) {
        const std::string tag = arms[i].tag;
        const fs::path appdata = work_dir / ("appdata-" + tag);
        // 每臂**全新私有 appdata**（复用旧存档会让读数不可复现；见 native_override_e2e 的同一条注释）
        fs::remove_all(appdata, ec);
        fs::create_directories(appdata, ec);
        if (ec) LIFT_SKIP("清不掉旧的私有 appdata");
        const fs::path frames = work_dir / ("frames-" + tag);
        fs::remove_all(frames, ec);
        fs::create_directories(frames, ec);
        const fs::path log_path = work_dir / ("run-" + tag + ".log");
        fs::remove(log_path, ec);

        // 诊断行（`[draw]`）只有 `LIFT_SLOW_DRAW=1` + 阈值 0 才打；它**不改变像素**。
        setEnvVar("LIFT_SLOW_DRAW", "1");
        setEnvVar("LIFT_SLOW_DRAW_MS", "0");
        if (arms[i].mip_on) setEnvVar("LIFT_NO_MIP", "");
        else setEnvVar("LIFT_NO_MIP", "1");

#if defined(_WIN32)
        std::vector<std::wstring> wargs_v;
        wargs_v.push_back(L"--game-dir");
        wargs_v.push_back(game_dir.wstring());
        wargs_v.push_back(L"--appdata");
        wargs_v.push_back(appdata.wstring());
        wargs_v.push_back(L"--skip-logo");
        wargs_v.push_back(L"--present");
        wargs_v.push_back(L"headless");
        wargs_v.push_back(L"--raster");
        wargs_v.push_back(L"cpu");
        wargs_v.push_back(L"--audio");
        wargs_v.push_back(L"off");
        wargs_v.push_back(L"--native-overrides");
        wargs_v.push_back(L"all");
        wargs_v.push_back(L"--max-frames");
        wargs_v.push_back(L"2");
        wargs_v.push_back(L"--dump-frame");
        wargs_v.push_back((frames / "frame_{frame}.bmp").wstring());
        std::wstring wargs;
        for (const std::wstring& a : wargs_v) wargs += L" " + quoteArg(a);
        const ChildResult r = runChild(exe, wargs, log_path);
#else
        std::vector<std::string> child;
        child.push_back("--game-dir"), child.push_back(game_dir.string());
        child.push_back("--appdata"), child.push_back(appdata.string());
        child.push_back("--skip-logo");
        child.push_back("--present"), child.push_back("headless");
        child.push_back("--raster"), child.push_back("cpu");
        child.push_back("--audio"), child.push_back("off");
        child.push_back("--native-overrides"), child.push_back("all");
        child.push_back("--max-frames"), child.push_back("2");
        child.push_back("--dump-frame"), child.push_back((frames / "frame_{frame}.bmp").string());
        std::string cargs;
        for (const std::string& a : child) cargs += " " + shellQuote(a);
        const ChildResult r = runChild(exe, cargs, log_path);
#endif
        setEnvVar("LIFT_SLOW_DRAW", "");
        setEnvVar("LIFT_SLOW_DRAW_MS", "");
        setEnvVar("LIFT_NO_MIP", "");
        if (!r.started) LIFT_SKIP("起不了 emulator-lift 子进程（exe 路径 / 沙箱限制？）");
        logs[i] = readAllBytes(log_path);
        if (logs[i].empty()) LIFT_SKIP("子进程没写日志（工作目录 / 权限问题）");
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(r.exit_code), 0);
        const fs::path frame2 = frames / "frame_0002.bmp";
        LIFT_CHECK(fs::exists(frame2));
        if (!fs::exists(frame2)) return 1;
        reads[i] = measureRegion(frame2);
        LIFT_CHECK(reads[i].valid);
    }

    const std::vector<DrawLine> on = parseDrawLines(logs[0]);
    const std::vector<DrawLine> off = parseDrawLines(logs[1]);
    // ② 开臂：真的取到了派生级、真的算了链
    std::size_t on_mipped = 0, on_rebuilt = 0, on_fullscreen_bg_lvl0 = 0, on_fullscreen_bg = 0;
    for (const DrawLine& d : on) {
        if (d.lvl_max >= 1u) ++on_mipped;
        if (d.miprb > 0) ++on_rebuilt;
        // ④ 全屏 2048² 背景层（放大采样）必须仍是 level 0
        const bool fullframe = d.bbox_x0 == 0 && d.bbox_y0 == 0 && d.bbox_x1 >= 1280 && d.bbox_y1 >= 720;
        if (fullframe && d.tex_w == 2048u && d.tex_h == 2048u) {
            ++on_fullscreen_bg;
            if (d.lvl_max == 0u) ++on_fullscreen_bg_lvl0;
        }
    }
    // ③ 关臂：全 0
    std::size_t off_mipped = 0, off_rebuilt = 0;
    for (const DrawLine& d : off) {
        if (d.lvl_max >= 1u) ++off_mipped;
        if (d.miprb > 0) ++off_rebuilt;
    }
    std::printf("mip-e2e: draws on=%zu off=%zu · on_mipped=%zu on_rebuilt=%zu · off_mipped=%zu off_rebuilt=%zu\n",
                on.size(), off.size(), on_mipped, on_rebuilt, off_mipped, off_rebuilt);
    std::printf("mip-e2e: 全屏 2048² 背景层 %zu 笔，其中 lvl=0..0 的 %zu 笔\n", on_fullscreen_bg,
                on_fullscreen_bg_lvl0);
    std::printf("mip-e2e: 右侧 (%d,%d) %dx%d 11x11 中值 TH=%d —— 开臂 density=%.3f%% row_hf=%.4f；"
                "关臂 density=%.3f%% row_hf=%.4f\n",
                kRegionX, kRegionY, kRegionW, kRegionH, kMedianThreshold, reads[0].density, reads[0].row_hf,
                reads[1].density, reads[1].row_hf);

    LIFT_CHECK(!on.empty());
    LIFT_CHECK(!off.empty());
    LIFT_CHECK(on_mipped >= 1u);    // 真有缩小批取到 level >= 1
    LIFT_CHECK(on_rebuilt >= 1u);   // 派生链真的被算过
    LIFT_CHECK(off_mipped == 0u);   // 对照臂真的关掉了
    LIFT_CHECK(off_rebuilt == 0u);
    // ④ 全屏 2048² 背景层里**至少有一笔**仍是 level 0（本 repro 的 page 23 是放大采样）：
    //   ⛔ 不要求"全部"—— 另一笔 page 22 的足迹恰好 ≈1:1（rho≈1.05），最近级落到 1 是正确的。
    LIFT_CHECK(on_fullscreen_bg_lvl0 >= 1u);

    return finish("mip-e2e");
}
