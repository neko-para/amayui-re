// emulator-lift · **GPU 黄金图回归**（`REQ-01M73G49T01W1CQDSME83ES0YD`）
//
// 口径（这份守卫回答什么）：把**刚跑出来的这一帧**与**上一次被人工接受的画面**（黄金图，入库走 LFS）
// 比 —— 不依赖 CPU 实现，也不依赖 `--gpu-strict` 那种"只看有没有回退"的账本。
// 红意味着什么：GPU 臂的画面**变了**（且变化超过容差），而"没崩 / 没回退 / strict 绿"都拦不住它。
//   ★ 典型样本 = S1（`REQ-01M73P3GXW22Y467YGYPQSM571`）：整层 Live2D 角色消失，账本全绿。
//
// 两层用例（同一个可执行，两种模式）：
//   ① **pure**（`--self-test`，CTest 名 `golden-image-compare`，**顶层无条件注册**）：
//      比较器 + PNG 编解码的机械判据 —— 合成两张 64×48 的 PNG：
//        · 自己比自己 ⇒ 必须**过**（不许自反判红）；
//        · 只改一个 16×16 块（每通道 +64）⇒ 必须**红**（块判据是局部缺陷的主抓点）；
//        · 全图统一 +1 LSB ⇒ 必须仍**过**（跨驱动 ≤1 LSB 的口径；防止容差被压成 0）；
//        · 尺寸不一致 ⇒ 必须**红**（⛔ 不拿缩放糊过去）。
//      ★ `tools/mutate-check.mjs` 的 `ctest:golden-image-compare` 指的就是这一层。
//   ② **端到端 fixture**（CTest 名 `golden-image`，**assets 档**：注册位置在 `if(EXISTS 安装树)` 里）：
//      起 `emulator-lift.exe` 跑 **TITLE 第 2 帧**（`--skip-logo --present headless --raster gpu
//      --audio off --native-overrides all --max-frames 2 --dump-frame <临时目录>`），把产出的 BMP
//      转成 RGBA，与 `tests/golden/title-f2-gpu.png` 比。
//
// 判据与容差（真源 = `runtime/golden_image.h` 的 `Tolerance`；这里只引用，⛔ 不另抄一份数字）：
//   ① 分块（16×16）块内 `max-channel |Δ|` 均值 ≤ 容差；
//   ② 全图 `mean |Δch|` ≤ 容差；
//   ③ 单像素 `max-channel |Δ|` > `outlier_delta` 的比例 ≤ `max_outlier_ratio`。
//   ★ **⛔ 不用整帧哈希当唯一判据**：实测同配置两次运行的亮度档数出现过 243 vs 248
//     （GPU 驱动 / 浮点路径的正常抖动）；哈希会把正常抖动报成回归。本机两次连跑实测见报告。
//   ★ 只比 R/G/B（`--dump-frame` 的 BMP 把 alpha 写成 0，见 `SurfaceArena::dumpBmp`）。
//
// `LIFT_UPDATE_GOLDEN=1`（**仅供人工确认后使用**）：用本次产出**覆盖**黄金图，并在日志里
//   **大声**打一行 `⚠ 已更新黄金图`（含路径 / 字节数 / 旧图对比读数）。⛔ 默认路径永远只读黄金图。
//
// 跳过规则（⛔ 不许静默通过；每条都打印原因）：
//   · 没有安装树（`<game-dir>/SYS4INI.BIN` 不存在）⇒ `LIFT_SKIP`(77)；
//   · GPU 设备不可用（`SdlWindow` 拿不到 SDL_GPU 设备）⇒ `LIFT_SKIP`(77) + 原因；
//   · 黄金图是 LFS 指针（没 `git lfs pull`）⇒ `LIFT_SKIP`(77) + 原因（⛔ 不当成画面回归）。
//
// ★ 子进程输出走**文件重定向**（⛔ 不走管道：受限沙箱里捕获管道会 EPERM），形制照 `tests/quit_tail.cpp`。
// ★ 子进程的 cwd 继承测试进程（CTest 把 WORKING_DIRECTORY 设成源码目录）⇒ 模拟器默认的执行源
//   `../../corpus/assets/age/AGE.EXE__dumped.sectfix.EXE` 能按相对路径找到。
// ★ `--dump-frame` 的**目录必须先建好**（已知坑：目录不存在会让进程空转到 `--time-limit`）⇒
//   本用例在起子进程之前 `create_directories(work/frames)`，并给出 `--time-limit 300` 兜底。
//
// 没做（本单只落一条 fixture，其余留"未做"）：`固定功能覆盖层（fade 黑幕）` / `sprite 批` /
//   `渲染到纹理（SetRenderTarget 到纹理后再 sprite）` 三条 fixture，以及跨驱动 / 跨机器的
//   容差标定（本机实测噪声为 0，余量只按"跨驱动 ≤1 LSB"给）。
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "check.h"
#include "runtime/golden_image.h"
#include "runtime/host_encoding.h"
#include "runtime/png_decode.h"
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

// ★ 路径 → **UTF-8**：`fs::path::string()` 在 Windows 上返回的是 **ANSI 代码页**字节
//   （中文 Windows = 936），而 `lift::utf8ToWide` 是**严格 UTF-8** 解码 ⇒ 含"天結"的
//   game-dir 会被解成空串、`--game-dir` 静默丢参（实测：子进程打"没有 --game-dir"、
//   40k 条指令就 `ExitProcess`，一帧都没 Present）。⇒ 子进程参数与日志里的路径一律走
//   `u8string()`（C++20 的 UTF-8 视图）。
std::string pathToUtf8(const fs::path& p) {
    const std::u8string u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

// ---------------------------------------------------------------------------------------
// BMP（`--dump-frame` 的产出）→ RGBA
// ---------------------------------------------------------------------------------------
// ★ 只认 `SurfaceArena::dumpBmp` 会写的形态：BITMAPFILEHEADER(14) + BITMAPINFOHEADER(≥40) +
//   BI_RGB + 24/32bpp + 行 4 字节对齐；高度**正数 = 自下而上**（BMP 惯例），负数 = 自上而下。
//   ⛔ 不做色彩管理 / 不认压缩格式：认不出来就返回 false + 一句原因（调用方判红）。
bool loadBmpRgba(const fs::path& path, lift::png::RgbaImage* out, std::string* why) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (why) *why = "打不开 BMP：" + pathToUtf8(path);
        return false;
    }
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() < 14u + 40u) {
        if (why) *why = "BMP 太短（连文件头都不够）";
        return false;
    }
    auto u16 = [&](std::size_t at) {
        return static_cast<std::uint16_t>(bytes[at] | (static_cast<std::uint16_t>(bytes[at + 1]) << 8));
    };
    auto u32 = [&](std::size_t at) {
        return static_cast<std::uint32_t>(bytes[at]) | (static_cast<std::uint32_t>(bytes[at + 1]) << 8) |
               (static_cast<std::uint32_t>(bytes[at + 2]) << 16) |
               (static_cast<std::uint32_t>(bytes[at + 3]) << 24);
    };
    if (bytes[0] != 'B' || bytes[1] != 'M') {
        if (why) *why = "不是 BMP（没有 BM 魔数）";
        return false;
    }
    const std::uint32_t data_offset = u32(10);
    const std::uint32_t info_size = u32(14);
    if (info_size < 40u) {
        if (why) *why = "不是 BITMAPINFOHEADER（info_size < 40）";
        return false;
    }
    const std::int32_t width = static_cast<std::int32_t>(u32(18));
    const std::int32_t raw_height = static_cast<std::int32_t>(u32(22));
    const std::uint16_t planes = u16(26);
    const std::uint16_t bpp = u16(28);
    const std::uint32_t compression = u32(30);
    if (width <= 0 || raw_height == 0 || planes != 1 || compression != 0 || (bpp != 24 && bpp != 32)) {
        if (why) *why = "BMP 形态不支持（宽度 / 平面数 / 压缩 / 位深）";
        return false;
    }
    const bool top_down = raw_height < 0;
    const std::uint32_t height = static_cast<std::uint32_t>(top_down ? -raw_height : raw_height);
    const std::uint32_t src_row = ((static_cast<std::uint32_t>(width) * (bpp / 8u)) + 3u) & ~3u;
    const std::uint64_t need =
        static_cast<std::uint64_t>(data_offset) + static_cast<std::uint64_t>(src_row) * height;
    if (need > bytes.size()) {
        if (why) *why = "BMP 像素区越过文件末尾（截断）";
        return false;
    }

    lift::png::RgbaImage img;
    img.width = static_cast<std::uint32_t>(width);
    img.height = height;
    img.rgba.resize(static_cast<std::size_t>(width) * height * 4u);
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint32_t src_y = top_down ? y : (height - 1u - y);
        const std::uint8_t* line = bytes.data() + data_offset + static_cast<std::uint64_t>(src_y) * src_row;
        for (std::uint32_t x = 0; x < static_cast<std::uint32_t>(width); ++x) {
            const std::uint32_t at = (y * img.width + x) * 4u;
            if (bpp == 32u) {
                const std::uint8_t* px = line + static_cast<std::size_t>(x) * 4u;
                img.rgba[at + 0] = px[2];  // BMP 是 BGR(A)
                img.rgba[at + 1] = px[1];
                img.rgba[at + 2] = px[0];
            } else {
                const std::uint8_t* px = line + static_cast<std::size_t>(x) * 3u;
                img.rgba[at + 0] = px[2];
                img.rgba[at + 1] = px[1];
                img.rgba[at + 2] = px[0];
            }
            // ★ `--dump-frame` 的 32bpp 把 alpha 字节写成 0（alpha 不参与比较，这里补成不透明，
            //   让"同一张图从 PNG 读回来"与"从 BMP 读回来"在 RGBA 层面同形）。
            img.rgba[at + 3] = 255;
        }
    }
    *out = std::move(img);
    return true;
}

// ---------------------------------------------------------------------------------------
// 子进程（stdout+stderr 落同一个文件；不捕获管道）
// ---------------------------------------------------------------------------------------
struct ChildResult {
    int exit_code = -1;
    bool started = false;
};

std::string readAllBytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string lastLines(const std::string& text, std::size_t max_lines) {
    std::vector<std::string> lines;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t nl = text.find('\n', pos);
        lines.push_back(text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    const std::size_t begin = lines.size() > max_lines ? lines.size() - max_lines : 0;
    std::string out;
    for (std::size_t i = begin; i < lines.size(); ++i) {
        out += lines[i];
        out += '\n';
    }
    return out;
}

#if defined(_WIN32)
std::wstring quoteArg(const std::wstring& s) { return L"\"" + s + L"\""; }

ChildResult runChild(const fs::path& exe, const std::vector<std::string>& args, const fs::path& log_path) {
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
    std::wstring cmd = quoteArg(exe.wstring());
    for (const std::string& a : args) cmd += L" " + quoteArg(lift::utf8ToWide(a));
    const BOOL ok = ::CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                     nullptr, &si, &pi);
    ::CloseHandle(h);
    if (!ok) return r;
    r.started = true;
    // 字幕：本 fixture 单次 run 实测 ~7 s（见报告）；给 5 分钟，卡住必须红、⛔ 不许挂死流水线。
    const DWORD waited = ::WaitForSingleObject(pi.hProcess, 300000u);
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
    std::string cmd = shellQuote(pathToUtf8(exe));
    for (const std::string& a : args) cmd += " " + shellQuote(a);
    cmd += " > " + shellQuote(pathToUtf8(log_path)) + " 2>&1";
    const int rc = std::system(cmd.c_str());
    if (rc == -1) return r;
    if (!fs::exists(log_path)) return r;
    r.started = true;
    r.exit_code = WIFEXITED(rc) ? WEXITSTATUS(rc) : (WIFSIGNALED(rc) ? 128 + WTERMSIG(rc) : 1);
    return r;
}
#endif

bool contains(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

// 环境不可用（**不是**判据红）：GPU / 执行源。判据与 `tests/quit_tail.cpp` 同一份（本用例是
//   `--present headless` ⇒ headless 档那条串）。
bool environmentUnavailable(const std::string& log) {
    return contains(log, "要求 GPU 光栅化") || contains(log, "--raster gpu（headless）失败") ||
           contains(log, "窗口打开失败") || contains(log, "找不到执行源") || contains(log, "拒绝启动");
}

// ---------------------------------------------------------------------------------------
// pure 用例：比较器 + PNG 编解码（不碰游戏 / GPU / 文件系统）
// ---------------------------------------------------------------------------------------
lift::png::RgbaImage syntheticImage(std::uint32_t w, std::uint32_t h) {
    lift::png::RgbaImage img;
    img.width = w;
    img.height = h;
    img.rgba.resize(static_cast<std::size_t>(w) * h * 4u);
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::size_t at = (static_cast<std::size_t>(y) * w + x) * 4u;
            img.rgba[at + 0] = static_cast<std::uint8_t>((x * 4u + y) & 0xFFu);
            img.rgba[at + 1] = static_cast<std::uint8_t>((x ^ y) & 0xFFu);
            img.rgba[at + 2] = static_cast<std::uint8_t>((x + y * 3u) & 0xFFu);
            img.rgba[at + 3] = 255;
        }
    }
    return img;
}

// PNG 往返：编码成字节 → 用**与端到端同一条**解码路径读回来。
bool pngRoundTrip(const lift::png::RgbaImage& in, lift::png::RgbaImage* out) {
    std::vector<std::uint8_t> bytes;
    std::string why;
    if (!lift::golden::encodeRgba8(in, &bytes, &why)) {
        std::fprintf(stderr, "PNG 编码失败：%s\n", why.c_str());
        return false;
    }
    if (bytes.empty() || bytes[0] != 0x89 || bytes[1] != 'P') {
        std::fprintf(stderr, "PNG 编码产出的字节不像 PNG\n");
        return false;
    }
    if (!lift::png::decodeRgba8(bytes.data(), bytes.size(), 0, out, &why)) {
        std::fprintf(stderr, "PNG 解码失败：%s\n", why.c_str());
        return false;
    }
    return true;
}

void pureCases() {
    using lift_test::g_current_case;
    const lift::golden::Tolerance tol;

    // ---- 用例 1：自己比自己 ⇒ 必须过（自反；⛔ 比较器不许恒红）----
    g_current_case = "golden/compare-self-passes";
    {
        const lift::png::RgbaImage base = syntheticImage(64, 48);
        lift::png::RgbaImage decoded;
        LIFT_CHECK(pngRoundTrip(base, &decoded));
        if (decoded.width == base.width) {  // 往返失败时后面的比较没有意义
            lift::golden::Report rep;
            const bool ok = lift::golden::compare(decoded, base, tol, &rep);
            std::printf("golden/compare-self: global=%.4f block_worst=%.4f outliers=%u max|Δch|=%u ⇒ %s\n",
                        rep.global_mean, rep.block_mean_worst, rep.outlier_pixels, rep.max_channel_diff,
                        ok ? "PASS" : "FAIL");
            LIFT_CHECK(ok);
            LIFT_CHECK(rep.size_ok && rep.block_ok && rep.global_ok && rep.outlier_ok);
            LIFT_CHECK(rep.differing_pixels == 0);
            LIFT_CHECK(rep.max_channel_diff == 0);
        }
    }

    // ---- 用例 2：只改一个 16×16 块（每通道 +64）⇒ 必须红，且红在**块判据**上 ----
    g_current_case = "golden/compare-one-block-fails";
    {
        const lift::png::RgbaImage base = syntheticImage(64, 48);
        lift::png::RgbaImage mutated = base;
        for (std::uint32_t y = 16; y < 32; ++y) {
            for (std::uint32_t x = 16; x < 32; ++x) {
                const std::size_t at = (static_cast<std::size_t>(y) * mutated.width + x) * 4u;
                for (std::size_t c = 0; c < 3; ++c) {
                    const int v = static_cast<int>(mutated.rgba[at + c]) + 64;
                    mutated.rgba[at + c] = static_cast<std::uint8_t>(v > 255 ? 255 : v);
                }
            }
        }
        lift::png::RgbaImage decoded;
        LIFT_CHECK(pngRoundTrip(mutated, &decoded));
        if (decoded.width == base.width) {
            lift::golden::Report rep;
            const bool ok = lift::golden::compare(decoded, base, tol, &rep);
            std::printf(
                "golden/compare-one-block: global=%.4f block_worst=%.4f @(%u,%u) failing=%u/%u ⇒ %s\n",
                rep.global_mean, rep.block_mean_worst, rep.worst_block_x, rep.worst_block_y,
                rep.failing_blocks, rep.blocks, ok ? "PASS（错！应该判红）" : "REJECTED（预期：必须红）");
            LIFT_CHECK(!ok);
            LIFT_CHECK(rep.size_ok);
            LIFT_CHECK(!rep.block_ok);  // ★ 块判据是局部缺陷的主抓点
            LIFT_CHECK(rep.failing_blocks >= 1);
            LIFT_CHECK(rep.worst_block_x == 16u && rep.worst_block_y == 16u);
        }
    }

    // ---- 用例 3：全图统一 +1 LSB ⇒ 必须仍过（跨驱动口径；防止容差被压成 0）----
    g_current_case = "golden/compare-uniform-1lsb-passes";
    {
        const lift::png::RgbaImage base = syntheticImage(64, 48);
        lift::png::RgbaImage one = base;
        for (std::size_t at = 0; at + 3 < one.rgba.size(); at += 4) {
            for (std::size_t c = 0; c < 3; ++c) {
                const int v = static_cast<int>(one.rgba[at + c]) + 1;
                one.rgba[at + c] = static_cast<std::uint8_t>(v > 255 ? 255 : v);
            }
        }
        lift::png::RgbaImage decoded;
        LIFT_CHECK(pngRoundTrip(one, &decoded));
        if (decoded.width == base.width) {
            lift::golden::Report rep;
            const bool ok = lift::golden::compare(decoded, base, tol, &rep);
            std::printf("golden/compare-1lsb: global=%.4f block_worst=%.4f ⇒ %s\n", rep.global_mean,
                        rep.block_mean_worst, ok ? "PASS" : "FAIL");
            LIFT_CHECK(ok);
            LIFT_CHECK(rep.global_mean > 0.0);  // 真的喂了差异（⛔ 不是两张一样的图）
        }
    }

    // ---- 用例 4：尺寸不一致 ⇒ 必须红（⛔ 不拿缩放 / 裁剪糊过去）----
    g_current_case = "golden/compare-size-mismatch-fails";
    {
        const lift::png::RgbaImage a = syntheticImage(64, 48);
        const lift::png::RgbaImage b = syntheticImage(48, 64);
        lift::golden::Report rep;
        const bool ok = lift::golden::compare(a, b, tol, &rep);
        LIFT_CHECK(!ok);
        LIFT_CHECK(!rep.size_ok);
    }
}

// ---------------------------------------------------------------------------------------
// 端到端 fixture：TITLE 第 2 帧（GPU 臂）
// ---------------------------------------------------------------------------------------
// TITLE 首帧之后第 2 个 Present：`--skip-logo` 已跳过 LOGO 影片 ⇒ 这一帧是 TITLE 画面
// （含 Live2D 角色层 —— S1 那条"整层丢角色"正好落在这里）。
constexpr const char* kFrameFile = "f0002.bmp";

int runFixture(const fs::path& exe, fs::path game_dir, const fs::path& work_dir, const fs::path& golden_png) {
    using lift_test::g_current_case;
    g_current_case = "golden-image/title-f2-gpu";

    // ★ 跳过规则必须排在**第一个 `LIFT_CHECK` 之前**：`LIFT_SKIP` 在已有失败时会 `return 1`
    //   （`tests/check.h`）⇒ 排在断言之后就变成"环境问题报 FAIL"。
    if (const char* env_game = std::getenv("LIFT_ASSETS_GAME_DIR");
        env_game != nullptr && *env_game != '\0') {
        game_dir = fs::path(lift::utf8ToWide(env_game));
        std::printf("golden-image: game-dir 被 LIFT_ASSETS_GAME_DIR 覆盖 ⇒ %s\n", pathToUtf8(game_dir).c_str());
    }
    // 同 CMake / quit_tail 的安装树判据：`SYS4INI.BIN` 在 ⇒ 是真的安装树。
    if (!fs::exists(game_dir / "SYS4INI.BIN")) {
        std::printf("golden-image: 安装树判据 = %s\n", pathToUtf8(game_dir / "SYS4INI.BIN").c_str());
        LIFT_SKIP("没有安装树（<game-dir>/SYS4INI.BIN 不存在）⇒ 本 fixture 不可用");
    }
    if (!fs::exists(exe)) LIFT_SKIP("找不到 emulator-lift.exe（CTest 的 $<TARGET_FILE:emulator-lift>？）");

    // GPU 设备探针（判据同 `tests/gpu_raster.cpp` / `tests/quit_tail.cpp`）：
    //   ⛔ 不拿 CPU 光栅化冒充 —— 那会把档位悄悄降级成"不是本守卫要测的路径"。
    {
        lift::SdlWindow probe;
        lift::SdlWindowOptions popt;
        popt.title = "emulator-lift golden-image gpu probe";
        popt.driver_list = "gpu";
        popt.hidden = true;
        popt.raster_gpu = true;
        std::string perr;
        if (!probe.init(popt, &perr)) {
            std::printf("golden-image: GPU 探针失败：%s\n", perr.c_str());
            LIFT_SKIP("GPU 设备不可用（SdlWindow::init 失败）⇒ 不是画面回归");
        }
        if (!probe.rasterActive()) {
            probe.shutdown();
            LIFT_SKIP("GPU 设备不可用（rasterActive == false，拿不到 SDL_GPU 设备）⇒ 不是画面回归");
        }
        probe.shutdown();
    }

    // 工作目录：**每次清空**（⛔ 留着上一次的 BMP 会让"这次没导出"也拿旧图比较 ⇒ 假绿）。
    std::error_code ec;
    fs::remove_all(work_dir, ec);
    const fs::path frames_dir = work_dir / "frames";
    fs::create_directories(frames_dir, ec);
    if (ec) LIFT_SKIP("建不了临时目录（权限 / 路径？）⇒ 环境问题");
    // ★ 已知坑：`--dump-frame` 的目录必须先建好，否则进程空转到 `--time-limit`。
    const fs::path dump_pattern = frames_dir / "f{frame}.bmp";
    const fs::path log_path = work_dir / "golden-image.log";
    const fs::path appdata = work_dir / "appdata";
    fs::create_directories(appdata, ec);

    std::vector<std::string> child = {
        "--game-dir", pathToUtf8(game_dir),
        "--appdata", pathToUtf8(appdata),
        "--skip-logo",
        "--present", "headless",
        "--raster", "gpu",
        "--audio", "off",
        "--native-overrides", "all",
        "--max-frames", "2",
        "--dump-frame", pathToUtf8(dump_pattern),
        "--time-limit", "300",
    };
    std::printf("golden-image: 起子进程：%s（日志 %s）\n", pathToUtf8(exe).c_str(), pathToUtf8(log_path).c_str());
    const ChildResult r = runChild(exe, child, log_path);
    if (!r.started) LIFT_SKIP("起不了 emulator-lift 子进程（exe 路径 / 沙箱限制？）");
    const std::string log = readAllBytes(log_path);
    if (log.empty()) LIFT_SKIP("子进程没写日志（工作目录 / 权限问题）");
    std::printf("golden-image: 子进程 exit=%d 日志 %zu 字节\n", r.exit_code, log.size());
    if (r.exit_code != 0 && environmentUnavailable(log)) {
        LIFT_SKIP("GPU / 执行源不可用 ⇒ 这条不是画面回归");
    }
    if (r.exit_code != 0) {
        std::printf("golden-image: 子进程日志尾部：\n%s", lastLines(log, 24).c_str());
    }
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(r.exit_code), 0u);

    const fs::path frame_path = frames_dir / kFrameFile;
    LIFT_CHECK(fs::exists(frame_path));
    if (!fs::exists(frame_path)) return lift_test::finish("golden-image");

    lift::png::RgbaImage actual;
    {
        std::string why;
        LIFT_CHECK(loadBmpRgba(frame_path, &actual, &why));
        if (!why.empty()) std::printf("golden-image: 读 BMP 失败：%s\n", why.c_str());
    }
    if (actual.width == 0) return lift_test::finish("golden-image");
    std::printf("golden-image: 本次产出 %ux%u（%s）\n", actual.width, actual.height,
                pathToUtf8(frame_path).c_str());

    // ---- `LIFT_UPDATE_GOLDEN=1`：用本次产出覆盖黄金图（仅供人工确认后使用）----
    if (const char* upd = std::getenv("LIFT_UPDATE_GOLDEN"); upd != nullptr && std::strcmp(upd, "1") == 0) {
        std::vector<std::uint8_t> png_bytes;
        std::string why;
        LIFT_CHECK(lift::golden::encodeRgba8(actual, &png_bytes, &why));
        if (!why.empty()) std::printf("golden-image: 编码 PNG 失败：%s\n", why.c_str());
        if (!png_bytes.empty()) {
            std::error_code wec;
            fs::create_directories(golden_png.parent_path(), wec);
            std::ofstream out(golden_png, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(png_bytes.data()),
                      static_cast<std::streamsize>(png_bytes.size()));
            out.close();
            LIFT_CHECK(fs::exists(golden_png));
            std::printf(
                "golden-image: ⚠⚠⚠ **已更新黄金图**（LIFT_UPDATE_GOLDEN=1）⇒ %s（%zu 字节）\n"
                "golden-image: ⚠⚠⚠ 这是**仅供人工确认后使用**的口子；确认前 ⛔ 不要把它当回归证据\n",
                pathToUtf8(golden_png).c_str(), png_bytes.size());
        }
        return lift_test::finish("golden-image");
    }

    // ---- 与黄金图比 ----
    if (!fs::exists(golden_png)) {
        std::printf("golden-image: 黄金图不存在：%s\n", pathToUtf8(golden_png).c_str());
        LIFT_CHECK(false);  // 入库件缺失 = 代码仓问题（⛔ 不是 SKIP）
        return lift_test::finish("golden-image");
    }
    const std::string golden_bytes = readAllBytes(golden_png);
    if (golden_bytes.rfind("version https://git-lfs.github.com/spec/", 0) == 0) {
        LIFT_SKIP("黄金图是 LFS 指针（没 git lfs pull）⇒ 不是画面回归");
    }
    lift::png::RgbaImage golden;
    {
        std::string why;
        if (!lift::png::decodeRgba8(reinterpret_cast<const std::uint8_t*>(golden_bytes.data()),
                                    golden_bytes.size(), 0, &golden, &why)) {
            std::printf("golden-image: 黄金图解码失败：%s（%s）\n", why.c_str(), pathToUtf8(golden_png).c_str());
            LIFT_CHECK(false);
            return lift_test::finish("golden-image");
        }
    }
    std::printf("golden-image: 黄金图 %ux%u（%s）\n", golden.width, golden.height,
                pathToUtf8(golden_png).c_str());

    const lift::golden::Tolerance tol;
    lift::golden::Report rep;
    const bool ok = lift::golden::compare(actual, golden, tol, &rep);
    std::printf(
        "golden-image: global_mean=%.4f (≤%.4f) block_worst=%.4f @(%u,%u) failing_blocks=%u/%u "
        "outliers=%u (%.4f%% ≤ %.4f%%) max|Δch|=%u differing=%u ⇒ %s\n",
        rep.global_mean, tol.global_mean, rep.block_mean_worst, rep.worst_block_x, rep.worst_block_y,
        rep.failing_blocks, rep.blocks, rep.outlier_pixels, rep.outlier_ratio * 100.0,
        tol.max_outlier_ratio * 100.0, rep.max_channel_diff, rep.differing_pixels, ok ? "PASS" : "FAIL");

    // 四层分开报（红的时候能看出是哪一层；判据是**与**，四条都过才算过）。
    LIFT_CHECK(rep.size_ok);
    LIFT_CHECK(rep.block_ok);
    LIFT_CHECK(rep.global_ok);
    LIFT_CHECK(rep.outlier_ok);
    LIFT_CHECK(ok);
    return lift_test::finish("golden-image");
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args = lift::argvUtf8(argc, argv);
    if (args.size() >= 2 && args[1] == "--self-test") {
        pureCases();
        return lift_test::finish("golden-image-compare");
    }
    if (args.size() >= 6 && args[1] == "--fixture") {
        // <emulator-lift> <game-dir> <work-dir> <golden-png>
        const fs::path exe = fs::path(lift::utf8ToWide(args[2]));
        const fs::path game_dir = fs::path(lift::utf8ToWide(args[3]));
        const fs::path work_dir = fs::path(lift::utf8ToWide(args[4]));
        const fs::path golden_png = fs::path(lift::utf8ToWide(args[5]));
        return runFixture(exe, game_dir, work_dir, golden_png);
    }
    std::fprintf(stderr,
                 "用法：\n"
                 "  lift-golden-image --self-test\n"
                 "  lift-golden-image --fixture <emulator-lift> <game-dir> <work-dir> <golden-png>\n");
    return 2;
}
