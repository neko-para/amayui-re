// emulator-lift · 帧导出**每帧失败**的行为（`REQ-01M73YBCC3DZ05Y9NSDTRKTMMY`）
//
// 判据（红了意味着什么）：
//   ① 帧文件写不出去（本例：`dump_path` 是一个**已存在的目录** ⇒ `fopen(…, "wb")` 失败）时，
//      `HeadlessPresenter::present()` **仍然返回 true** —— 帧导出是宿主诊断，⛔ 不许让 guest 的
//      Present 失败。旧实现返回 false ⇒ `d3d9::present` 在 `--max-frames` 停机检查之前退出 ⇒
//      目录不存在时进程空转到别的停止条件（实测 546 s vs 目录建好后 7 s）。
//   ② 失败**计数**每次 +1（`dumpFailures()`；`main` 在 run summary 后打 `dumpFailures=`）。
//   ③ 默认级别 warn **只打一次**（第 2 次失败不再刷屏）：捕获 stderr，数"帧导出失败"出现次数 == 1。
//   ④ 写成功时照旧：返回 true、`lastDump()` 更新、失败计数不变（⛔ 别为了"不失败"把导出关掉）。
//
// ★ 为什么在 presenter 层守：CLI 的 pure 档只能跑自造 PE（没有 Present），覆盖不到"每帧"；
//   真的启动游戏是本单的 assets 档（一次几分钟），⛔ 不要把它塞进默认门禁。
// ★ 夹具：裸 `GuestMemory` + `SurfaceArena` 的 4×4 表面（不碰游戏 / LFS / SDL / 窗口）。
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <io.h>
#include <windows.h>
#endif

#include "check.h"

#include "runtime/guest_token.h"
#include "runtime/host_encoding.h"
#include "runtime/memory.h"
#include "runtime/present.h"
#include "runtime/surface.h"

namespace {

#if defined(_WIN32)
namespace fs = std::filesystem;

std::string readAllBytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::size_t countOccurrences(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return 0;
    std::size_t n = 0;
    for (std::size_t pos = hay.find(needle); pos != std::string::npos; pos = hay.find(needle, pos + needle.size())) {
        ++n;
    }
    return n;
}
#endif

}  // namespace

int main() {
    using namespace lift_test;

#if !defined(_WIN32)
    LIFT_SKIP("本用例的 stderr 捕获 / 目录当文件写用 MSVC 的 <io.h>（Windows 专有）");
#else
    g_current_case = "frame-dump/per-frame-failure";

    lift::GuestMemory mem;
    LIFT_CHECK(mem.reserve());
    lift::SurfaceArena arena;
    LIFT_CHECK(arena.init(&mem, 0x28000000u, 0x00400000u));

    std::uint32_t ea = 0;
    const lift::GuestSurface* surface = nullptr;
    {
        // SurfaceArena 的簿记入口要持令牌（`guest_token.h`）；否则会记一条违规（那是别的守卫的账）。
        lift::GuestToken::Guard guard(lift::guestToken());
        ea = arena.create(4, 4, lift::SurfaceFormat{});
        if (ea != 0) surface = arena.find(ea);
    }
    LIFT_CHECK_NE_U32(ea, 0u);
    LIFT_CHECK(surface != nullptr);
    if (surface == nullptr) return lift_test::finish("frame-dump-failure");

    std::error_code ec;
    const fs::path root =
        fs::temp_directory_path(ec) / ("lift-frame-dump-failure-" + std::to_string(::GetCurrentProcessId()));
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    LIFT_CHECK(!ec);
    // ① 目标是一个**目录**：`fopen(dir, "wb")` 必然失败（且与权限无关 ⇒ 用例可复现）。
    const fs::path dir_target = root / "is-a-directory";
    fs::create_directories(dir_target, ec);
    LIFT_CHECK(!ec);

    // ★ `present()` / `lastDump()` 的路径口径是 **UTF-8**（与 `--dump-frame` / `argvUtf8` 一致）；
    //   `fs::path::string()` 在 Windows 上是窄编码（ANSI 码页）⇒ `%TEMP%` 含中文时会把非 UTF-8
    //   字节喂进去。⇒ 统一走 `lift::wideToUtf8`（与 `openHostFileUtf8` 同一口径）。
    const std::string dir_target_utf8 = lift::wideToUtf8(dir_target.c_str());

    lift::HeadlessPresenter presenter;

    // ---- 捕获 stderr：数 warn 的次数（② 计数 / ③ 只打一次）----
    const fs::path cap_path = root / "stderr.txt";
    const int saved_fd = _dup(_fileno(stderr));
    LIFT_CHECK(saved_fd >= 0);
    std::FILE* cap = nullptr;
    if (saved_fd >= 0) {
        // `_wfreopen` 是 deprecated（本仓口径是不留废弃告警）⇒ 用 `_wfreopen_s`；路径走宽字符
        // （临时目录可能含非 ASCII）。
        const errno_t rc = _wfreopen_s(&cap, cap_path.wstring().c_str(), L"w", stderr);
        if (rc != 0) cap = nullptr;
    }
    LIFT_CHECK(cap != nullptr);

    std::string err1;
    std::string err2;
    const bool ok1 = presenter.present(arena, *surface, dir_target_utf8, &err1);
    const bool ok2 = presenter.present(arena, *surface, dir_target_utf8, &err2);
    std::fflush(stderr);
    if (saved_fd >= 0) {
        _dup2(saved_fd, _fileno(stderr));
        _close(saved_fd);
    }
    const std::string captured = readAllBytes(cap_path);

    LIFT_CHECK(ok1);  // ① 帧导出失败**不**让 Present 失败
    LIFT_CHECK(ok2);
    LIFT_CHECK(!err1.empty());  // 原因仍交给调用方（诊断）
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(presenter.dumpFailures()), 2u);  // ② 每帧都计数
    // ③ 默认级别 warn 只出现一次（第 2 次只计数，不刷屏）
    const std::size_t warns = countOccurrences(captured, "帧导出失败");
    if (warns != 1) std::printf("frame-dump-failure: 捕获到的 warn 次数 = %zu（期望 1）\n", warns);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(warns), 1u);

    // ---- ④ 写成功时照旧：返回 true + lastDump 更新 + 失败计数不变 ----
    g_current_case = "frame-dump/per-frame-success";
    const fs::path good = root / "ok.bmp";
    const std::string good_utf8 = lift::wideToUtf8(good.c_str());
    std::string err3;
    LIFT_CHECK(presenter.present(arena, *surface, good_utf8, &err3));
    LIFT_CHECK(presenter.lastDump() == good_utf8);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(presenter.dumpFailures()), 2u);
    {
        // 用 `std::ifstream`（接受 `fs::path`，Windows 上走宽字符）：⛔ 窄 `fopen` 会按 ANSI 码页解释路径，
        // 而且在本仓的编译口径下会触发 CRT 的 deprecated 告警。
        std::ifstream in(good, std::ios::binary);
        LIFT_CHECK(static_cast<bool>(in));
        if (in) {
            unsigned char hdr[2] = {0, 0};
            in.read(reinterpret_cast<char*>(hdr), static_cast<std::streamsize>(sizeof(hdr)));
            const std::streamsize n = in.gcount();
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(n), 2u);
            LIFT_CHECK_EQ_U32(hdr[0], 'B');
            LIFT_CHECK_EQ_U32(hdr[1], 'M');
        }
    }

    {
        lift::GuestToken::Guard guard(lift::guestToken());
        LIFT_CHECK(arena.destroy(ea));
    }
    fs::remove_all(root, ec);
#endif

    return lift_test::finish("frame-dump-failure");
}
