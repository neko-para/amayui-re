// emulator-lift · **AOT 臂**的 `--overlay` 端到端（真子进程 + 真窗口像素）
//
// 判据（红了意味着什么）：
//   ① `--present window --present-hidden --overlay --max-frames 2` ⇒ **exit 0**（M0 判据 1）。
//      红了 ⇒ AOT 臂的 `--overlay` 参数面 / 接线没通（用户给了参数却跑不起来）。
//   ② `--present-screenshot` 的窗口 BMP 在**画布原点**（1280×720 帧 / 窗口 ⇒ (0,0)）是
//      HUD 底色 `#101820`（BMP 里是 BGR = 20 18 10）；同一条命令**不带** `--overlay` 时
//      那个像素是 guest 后缓冲的颜色（首帧 = 黑）。红了 ⇒ 叠层没有真的上屏，或原点错了。
//   ③ `--dump-frame` 的 guest 帧 BMP 在开 / 关 `--overlay` 两次运行里**逐字节相同**（M0 判据 4）。
//      红了 ⇒ UI 被写进了 guest 像素 / 后缓冲（那是硬边界：⛔ 不写 guest）。
//      ★ 口径：用 **1 帧** 运行做对照 —— 2 帧时第二帧的画面内容随真实时钟变化，两轮之间本来就
//        不同（实测：`--max-frames 2` 两轮 dump 不同；1 帧两轮 dump 逐字节相同）。
//        判据测的是"叠层有没有改 guest 像素"，不是"游戏画面确定不确定"；另跑一条**纯对照复制**
//        （第 4 次 run），若两条纯命令自己就不同 ⇒ 跳过逐字节判据（⛔ 不假红）。
//   ④ 收口摘要的 `present : … lastHash=0x…` 两次相同（M0 判据 4 的 arena.hash 面；同样用 1 帧）。
//   ⑤ 整轮 stderr 里没有 `[overlay]` / `[ui]` 的**周期行**（M0 判据 5；一次性收口行不算）。
//      ★ 形状：这两类前缀出现次数 ≤ 1（允许将来一条一次性 readout），⛔ 不是"字符串不出现"。
//
// ★ 缺 GPU / 生成物 / 执行源 / 安装树（`--game-dir`）/ 宿主字体 ⇒ `LIFT_SKIP(77)`（⛔ 不假绿）。
// ★ 子进程 cwd / `--appdata` 都指到临时目录 ⇒ 不往仓库 / 真 AppData 写东西。
// ★ 注册：顶层 `add_test` + `SKIP_RETURN_CODE 77`（与 `aot-audio-wiring` 同形；输入假定在场）。
//   ⚠ 子进程用 `CreateProcessW`（Windows 专有）⇒ 其它平台 `LIFT_SKIP`。
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
    std::wstring cmd = lift_test::quoteArg(exe.wstring()) + L" " + args;
    const BOOL ok = ::CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                     cwd.c_str(), &si, &pi);
    ::CloseHandle(h_out);
    ::CloseHandle(h_err);
    if (!ok) return r;
    r.started = true;
    const DWORD waited = ::WaitForSingleObject(pi.hProcess, 60000u);
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

/** 第一行以 `prefix` 开头的行（没有 ⇒ 空串）。 */
std::string firstLineStartingWith(const std::string& text, const std::string& prefix) {
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

/** 从 `present   : … lastHash=0x…` 里取 `lastHash=0x…` 这一段（没有 ⇒ 空串）。 */
std::string lastHashField(const std::string& out) {
    const std::string line = firstLineStartingWith(out, "present   : ");
    const std::size_t at = line.find("lastHash=");
    if (at == std::string::npos) return {};
    const std::size_t end = line.find(' ', at);
    return line.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

std::size_t countOccurrences(const std::string& text, const std::string& needle) {
    std::size_t n = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) ++n;
    return n;
}

struct BmpPixel {
    bool ok = false;
    std::uint8_t b = 0;
    std::uint8_t g = 0;
    std::uint8_t r = 0;
};

std::uint32_t le32(const std::string& b, std::size_t off) {
    if (b.size() < off + 4) return 0xFFFFFFFFu;
    return static_cast<std::uint32_t>(static_cast<unsigned char>(b[off])) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b[off + 1])) << 8) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b[off + 2])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b[off + 3])) << 24);
}

std::uint16_t le16(const std::string& b, std::size_t off) {
    if (b.size() < off + 2) return 0xFFFFu;
    return static_cast<std::uint16_t>(static_cast<unsigned char>(b[off]) |
                                      (static_cast<unsigned char>(b[off + 1]) << 8));
}

/** BMP（`SDL_SaveBMP` 产物：24/32bpp、自底向上）的 (x,y)，y 从图像**顶**数。 */
BmpPixel bmpPixel(const std::string& bytes, int x, int y) {
    BmpPixel p;
    if (bytes.size() < 54 || bytes[0] != 'B' || bytes[1] != 'M') return p;
    const std::uint32_t data_off = le32(bytes, 10);
    const std::int32_t w = static_cast<std::int32_t>(le32(bytes, 18));
    const std::int32_t h = static_cast<std::int32_t>(le32(bytes, 22));
    const std::uint16_t bpp = le16(bytes, 28);
    if (w <= 0 || h == 0 || (bpp != 24 && bpp != 32)) return p;
    const int height = h < 0 ? -h : h;
    if (x < 0 || y < 0 || x >= w || y >= height) return p;
    const int bytes_per_px = bpp / 8;
    const std::size_t stride = ((static_cast<std::size_t>(w) * bytes_per_px + 3u) / 4u) * 4u;
    const int row = h > 0 ? height - 1 - y : y;  // 正高度 = 自底向上
    const std::size_t at = data_off + static_cast<std::size_t>(row) * stride +
                           static_cast<std::size_t>(x) * static_cast<std::size_t>(bytes_per_px);
    if (at + 2 >= bytes.size()) return p;
    p.b = static_cast<std::uint8_t>(bytes[at + 0]);
    p.g = static_cast<std::uint8_t>(bytes[at + 1]);
    p.r = static_cast<std::uint8_t>(bytes[at + 2]);
    p.ok = true;
    return p;
}

#endif  // _WIN32

}  // namespace

int main(int argc, char** argv) {
    using namespace lift_test;

#if !defined(_WIN32)
    (void)argc;
    (void)argv;
    LIFT_SKIP("AOT 臂的端到端用 CreateProcessW 起子进程（Windows 专有）");
#else
    if (argc < 4) {
        LIFT_SKIP("需要 CTest 传 <emulator-lift-aot.exe> <AGE.EXE__dumped.sectfix.EXE> <安装树>（见 CMakeLists）");
    }
    const fs::path exe = fs::path(argv[1]);
    const fs::path age_exe = fs::path(argv[2]);
    const fs::path game_dir = fs::path(argv[3]);
    if (!fs::exists(exe)) LIFT_SKIP("emulator-lift-aot.exe 不在场");
    if (!fs::exists(age_exe)) LIFT_SKIP("执行源（corpus/assets/age/AGE.EXE__dumped.sectfix.EXE）不在场");
    if (!fs::exists(game_dir)) LIFT_SKIP("安装树（--game-dir）不在场：没有 AGERC.DLL 就跑不到 Present");

    std::error_code ec;
    const fs::path root =
        fs::temp_directory_path(ec) / ("lift-aot-overlay-" + std::to_string(::GetCurrentProcessId()));
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    LIFT_CHECK(!ec);

    const std::wstring exe_arg = L"--exe " + quoteArg(age_exe.wstring());
    const std::wstring game_arg = L" --game-dir " + quoteArg(game_dir.wstring());
    const wchar_t* const kCommon =
        L" --skip-logo --present window --present-hidden --time-limit 60";

    // ★ 四次 run：① 判据 1（两帧，exit 0）；②/③ 用 **1 帧** 做开/关对照；④ 是 ③ 的**纯对照复制**
    //   （同一命令再跑一遍）：③ 与 ④ 若不相同 ⇒ 这个 run 本身非确定 ⇒ 跳过逐字节判据（⛔ 不假红）；
    //   ③ 与 ④ 相同而 ② 不同 ⇒ 真的是 UI 改写了 guest 后缓冲（红）。
    struct Run {
        const char* name;
        int frames;
        bool overlay;
        bool artifacts;
    };
    const Run kRuns[] = {
        {"two-frames-overlay", 2, true, false},
        {"one-frame-overlay", 1, true, true},
        {"one-frame-plain", 1, false, true},
        {"one-frame-plain-control", 1, false, true},
    };
    struct Out {
        ChildRun run;
        fs::path shot;
        fs::path dump;
    };
    std::vector<Out> outs;
    for (const Run& k : kRuns) {
        g_current_case = k.name;
        const fs::path appdata = root / (std::string("appdata-") + k.name);
        fs::create_directories(appdata, ec);
        std::wstring args = exe_arg + game_arg + kCommon + L" --max-frames " +
                            std::to_wstring(k.frames) + L" --appdata " + quoteArg(appdata.wstring());
        Out o;
        o.shot = root / (std::string(k.name) + "-shot.bmp");
        o.dump = root / (std::string(k.name) + "-dump.bmp");
        if (k.artifacts) {
            args += L" --present-screenshot " + quoteArg(o.shot.wstring());
            args += L" --dump-frame " + quoteArg(o.dump.wstring());
        }
        if (k.overlay) args += L" --overlay";
        const std::string tag = std::string(k.name) + ".";
        o.run = runChild(exe, args, root, root / (tag + "out"), root / (tag + "err"));
        LIFT_CHECK(o.run.started);
        if (!o.run.started) continue;
        LIFT_CHECK(!o.run.timed_out);
        if (o.run.timed_out) continue;
        // 输入不在场 ⇒ skip（⛔ 不当红）。
        if (o.run.out.find("没有生成物") != std::string::npos) LIFT_SKIP("构建期没有生成物（缺 clang / 执行源 / closure）");
        if (o.run.err.find("没有可用的 GPU 光栅化后端") != std::string::npos) LIFT_SKIP("本机没有可用的 GPU 光栅化后端");
        if (o.run.err.find("宿主字体不可用") != std::string::npos) LIFT_SKIP("缺宿主字体（LIFT_HOST_FONT_PATH）");
        // ① 参数面 / 接线：每条命令都必须 exit 0（尤其 `--max-frames 2 --overlay`）。
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(o.run.exit_code), 0u);
        if (o.run.exit_code != 0) {
            std::fprintf(stderr, "  [%s] exit=%lu\n  stderr:\n%s\n", k.name,
                         static_cast<unsigned long>(o.run.exit_code), o.run.err.c_str());
        }
        // ⑤ 无周期行：`[overlay]` / `[ui]` 各至多一条（一次性 readout 允许）。
        LIFT_CHECK(countOccurrences(o.run.err, "[overlay]") <= 1);
        LIFT_CHECK(countOccurrences(o.run.err, "[ui]") <= 1);
        outs.push_back(std::move(o));
    }
    if (outs.size() != 4) return finish("aot-overlay");
    if (outs[0].run.exit_code != 0) return finish("aot-overlay");

    // ③/④ 的确定性前提：两轮**同一条纯命令**必须逐字节相同；不同 ⇒ 这个 run 非确定（⛔ 不假红）。
    {
        const std::string control_a = readAllBytes(outs[2].dump);
        const std::string control_b = readAllBytes(outs[3].dump);
        if (!control_a.empty() && control_a != control_b) {
            LIFT_SKIP("这个 fixture 的 guest 画面不是逐字节确定的（两轮纯对照 dump 不同）⇒ 逐字节判据不可判定");
        }
    }

    // ② 窗口截图：叠层真的上屏，且落在画布原点 (0,0)。
    const std::string shot_overlay = readAllBytes(outs[1].shot);
    const std::string shot_plain = readAllBytes(outs[2].shot);
    LIFT_CHECK(!shot_overlay.empty());
    LIFT_CHECK(!shot_plain.empty());
    const BmpPixel with_overlay = bmpPixel(shot_overlay, 0, 0);
    const BmpPixel without_overlay = bmpPixel(shot_plain, 0, 0);
    LIFT_CHECK(with_overlay.ok);
    LIFT_CHECK(without_overlay.ok);
    std::printf("shot(0,0): overlay=(%u,%u,%u) plain=(%u,%u,%u)\n", static_cast<unsigned>(with_overlay.r),
                static_cast<unsigned>(with_overlay.g), static_cast<unsigned>(with_overlay.b),
                static_cast<unsigned>(without_overlay.r), static_cast<unsigned>(without_overlay.g),
                static_cast<unsigned>(without_overlay.b));
    // `kOverlayBg = {16,24,32}` ⇒ BMP 的 BGR 字节 = 32,24,16。
    LIFT_CHECK_EQ_U32(with_overlay.b, 32u);
    LIFT_CHECK_EQ_U32(with_overlay.g, 24u);
    LIFT_CHECK_EQ_U32(with_overlay.r, 16u);
    LIFT_CHECK(with_overlay.b != without_overlay.b || with_overlay.g != without_overlay.g ||
               with_overlay.r != without_overlay.r);

    // ③ guest 帧逐字节相同（⛔ UI 不进 guest 后缓冲）。
    const std::string dump_overlay = readAllBytes(outs[1].dump);
    const std::string dump_plain = readAllBytes(outs[2].dump);
    LIFT_CHECK(!dump_overlay.empty());
    LIFT_CHECK(dump_overlay == dump_plain);

    // ④ arena.hash（最后一次 Present 的后缓冲哈希）两次相同。
    const std::string hash_overlay = lastHashField(outs[1].run.out);
    const std::string hash_plain = lastHashField(outs[2].run.out);
    std::printf("lastHash: overlay=%s plain=%s\n", hash_overlay.c_str(), hash_plain.c_str());
    LIFT_CHECK(!hash_overlay.empty());
    LIFT_CHECK(hash_overlay == hash_plain);
#endif

    return finish("aot-overlay");
}
