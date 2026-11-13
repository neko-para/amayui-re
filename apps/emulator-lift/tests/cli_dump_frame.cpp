// emulator-lift · `--dump-frame` 目录的 CLI 级守卫（`REQ-01M73YBCC3DZ05Y9NSDTRKTMMY`）
//
// 判据（红了意味着什么）：
//   ① `--dump-frame <不存在的目录>/frame_{frame}.bmp` 必须在**开跑之前**把目录建好
//      （子进程 exit 0 + `fs::is_directory(<目录>)`）；⛔ 不允许"跑起来才发现写不进去"。
//   ② 目录**建不出来**（本例：路径中间被一个普通文件挡住）⇒ `exit 2` + stderr 说清是
//      `--dump-frame` 且含"目录"字样；⛔ 不静默继续、⛔ 不留一行运行摘要（是启动前拒绝）。
//   ③ 两条子进程都必须在 **30 s 内**结束（`runChild` 的 `WaitForSingleObject(…, 30000)` +
//      `timed_out` 断言）—— 这条钉的正是本单的缺陷形态：目录不存在时进程空转 546 s。
//
// ★ 为什么是 CLI 级 + 真子进程：缺陷在**参数解析 → 目录创建 → 运行 → 退出码**这条链路上；
//   单测 `create_directories` 抓不到"根本没调用它 / 调用了却继续跑"。
// ★ 为什么 pure 档（顶层无条件注册）：执行源是本用例自己构造的**最小 PE32**（`nop` 一条）+
//   `--max-insns 1` ⇒ `stop: max-insns`，不碰 LFS / 游戏安装 / 窗口（与 `cli_time_limit.cpp`
//   同一口径）。★ 代价：自造 PE 不会 Present，所以本用例只覆盖**开跑前**那道闸；
//   "每帧写失败不拖住 Present" 由 `tests/frame_dump_failure.cpp` 在 presenter 层守。
//   ⛔ 不要为了覆盖 546 s 的空转而真的启动游戏 —— 那是 assets 档，且一次要跑几分钟。
// ★ 子进程的 cwd 指到临时目录：默认的 `../../corpus/...` 执行源 / `.lst` 候选都找不到，
//   结果不随宿主 cwd 漂（与 `cli_time_limit.cpp` 同一条纪律）。
//
// ★ 修前红（实测）：本用例的 ① 在修前返回 exit 0 但**目录不存在**（红）；② 返回 exit 0
//   并跑完最小 PE（红）。真游戏下的 546 s 空转读数见需求单 / `.tmp/reveq/…/report.md`
//   （目录建好后同一命令 7 s）。
#include <cstdint>
#include <cstdio>
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
#endif

namespace {

#if defined(_WIN32)

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------------------
// 子进程：stdout / stderr 各自重定向到**文件**（⛔ 不走管道：受限沙箱里捕获管道会 EPERM）
// ---------------------------------------------------------------------------------------
struct ChildRun {
    DWORD exit_code = 0xFFFFFFFFu;
    bool started = false;
    bool timed_out = false;
    double elapsed_ms = 0.0;
    std::string out;
    std::string err;
};

std::string readAllBytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::wstring quoteArg(const std::wstring& s) { return L"\"" + s + L"\""; }

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
    // ★ 30 s 上限：正常一次启动 + 一条指令 << 1 s；卡住必须**红**，⛔ 不许挂死流水线。
    const ULONGLONG t0 = ::GetTickCount64();
    const DWORD waited = ::WaitForSingleObject(pi.hProcess, 30000u);
    r.elapsed_ms = static_cast<double>(::GetTickCount64() - t0);
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

// ---------------------------------------------------------------------------------------
// 最小 PE32：一节 `.text`（RVA 0x1000，入口处一条 `nop`）。载入器只认节表 ⇒ 够跑 1 条指令。
//   ⛔ 不引入 PE 之外的依赖：`--max-insns 1` 之后运行必停（`stop : max-insns`），不碰任何导入。
// ---------------------------------------------------------------------------------------
std::vector<std::uint8_t> minimalPe() {
    const std::size_t kPeOff = 0x40;
    const std::size_t kOpt = kPeOff + 24;      // 0x58
    const std::size_t kOptSize = 0xE0;
    const std::size_t kSec = kOpt + kOptSize;  // 0x138
    std::vector<std::uint8_t> b(0x400, 0);
    auto w16 = [&b](std::size_t off, std::uint16_t v) { std::memcpy(b.data() + off, &v, sizeof(v)); };
    auto w32 = [&b](std::size_t off, std::uint32_t v) { std::memcpy(b.data() + off, &v, sizeof(v)); };

    b[0] = 'M';
    b[1] = 'Z';
    w32(0x3C, static_cast<std::uint32_t>(kPeOff));  // e_lfanew
    b[kPeOff + 0] = 'P';
    b[kPeOff + 1] = 'E';
    // COFF 头
    w16(kPeOff + 4, 0x014Cu);   // Machine = i386
    w16(kPeOff + 6, 1u);        // NumberOfSections
    w16(kPeOff + 20, static_cast<std::uint16_t>(kOptSize));  // SizeOfOptionalHeader
    w16(kPeOff + 22, 0x0102u);  // EXECUTABLE_IMAGE | 32BIT_MACHINE
    // PE32 optional header
    w16(kOpt + 0, 0x010Bu);     // Magic = PE32
    w32(kOpt + 16, 0x1000u);    // AddressOfEntryPoint
    w32(kOpt + 28, 0x00400000u);// ImageBase
    w32(kOpt + 56, 0x2000u);    // SizeOfImage
    w32(kOpt + 60, 0x200u);     // SizeOfHeaders（= 节 raw 起点，头里不留空洞）
    w32(kOpt + 72, 0x100000u);  // SizeOfStackReserve
    // 节表（一节）
    std::memcpy(b.data() + kSec, ".text", 5);
    w32(kSec + 8, 0x1000u);     // VirtualSize
    w32(kSec + 12, 0x1000u);    // VirtualAddress
    w32(kSec + 16, 0x200u);     // SizeOfRawData
    w32(kSec + 20, 0x200u);     // PointerToRawData
    w32(kSec + 36, 0x60000020u);// CODE | EXECUTE | READ
    b[0x200] = 0x90u;           // nop
    return b;
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

#endif  // _WIN32

}  // namespace

int main(int argc, char** argv) {
    using namespace lift_test;

#if !defined(_WIN32)
    (void)argc;
    (void)argv;
    LIFT_SKIP("--dump-frame 的 CLI 守卫用 CreateProcessW 起子进程（Windows 专有）");
#else
    if (argc < 2) {
        LIFT_SKIP("需要 CTest 传 <emulator-lift.exe>（见 CMakeLists 的 cli-dump-frame）");
    }
    const fs::path exe = fs::path(argv[1]);
    LIFT_CHECK(fs::exists(exe));

    std::error_code ec;
    const fs::path root =
        fs::temp_directory_path(ec) / ("lift-cli-dump-frame-" + std::to_string(::GetCurrentProcessId()));
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    LIFT_CHECK(!ec);

    // 执行源：自造的最小 PE（⛔ 不依赖 LFS / 游戏安装）
    const fs::path fake_exe = root / "lift-minimal.exe";
    {
        const std::vector<std::uint8_t> pe = minimalPe();
        std::ofstream out(fake_exe, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(pe.data()), static_cast<std::streamsize>(pe.size()));
        out.close();
        LIFT_CHECK(fs::exists(fake_exe));
    }

    // 公共尾巴：显式 `--exe` / `--game-dir`，`--audio off`（不碰真设备），`--max-insns 1`
    //   ⇒ 运行**必停**在一条指令之后，整条命令 < 1 s。
    const std::wstring common = L"--exe " + quoteArg(fake_exe.wstring()) + L" --game-dir " +
                                quoteArg(root.wstring()) + L" --audio off --max-insns 1 --max-frames 1";

    auto run = [&](const char* tag, const std::wstring& extra) {
        const fs::path o = root / (std::string(tag) + ".out");
        const fs::path e = root / (std::string(tag) + ".err");
        const std::wstring args = common + L" " + extra;
        return runChild(exe, args, root, o, e);
    };

    // ---- ① 不存在的目录：必须在开跑前建好，进程正常结束 ----
    g_current_case = "creates-missing-dir";
    const fs::path created_dir = root / "created" / "deep";
    const fs::path created_pattern = created_dir / "frame_{frame}.bmp";
    const ChildRun created = run("created", L"--dump-frame " + quoteArg(created_pattern.wstring()));
    LIFT_CHECK(created.started);
    LIFT_CHECK(!created.timed_out);  // ★ 30 s 内结束
    // ★ 无 GPU（含 `SDL_VIDEODRIVER=dummy`）：子进程在跑 guest 之前就拒绝启动 ⇒ SKIP 77。
    LIFT_SKIP_IF_NO_GPU(created.exit_code, created.err);
    LIFT_CHECK_EQ_U32(created.exit_code, 0u);
    // 判据：目录真的建出来了（去掉 create_directories ⇒ 这一条当场红）。
    LIFT_CHECK(fs::is_directory(created_dir));
    // 诊断行照旧在（本轮 0 帧 ⇒ 0 次失败）；⛔ 不要让"有这行"变成"导出成功"的替代品。
    LIFT_CHECK(hasLineStartingWith(created.out, "dumpFailures=0"));

    // ---- ② 建不出来：中间被一个普通文件挡住 ⇒ exit 2 + stderr 说清原因，且不进入 run ----
    g_current_case = "rejects-uncreatable-dir";
    {
        std::ofstream blocked(root / "blocked", std::ios::binary | std::ios::trunc);
        blocked << "not a directory";
        blocked.close();
    }
    const fs::path bad_pattern = root / "blocked" / "sub" / "frame.bmp";
    const ChildRun bad = run("blocked", L"--dump-frame " + quoteArg(bad_pattern.wstring()));
    LIFT_CHECK(bad.started);
    LIFT_CHECK(!bad.timed_out);  // ★ 30 s 内结束（启动前就拒绝）
    LIFT_CHECK_EQ_U32(bad.exit_code, 2u);
    LIFT_CHECK(bad.err.find("--dump-frame") != std::string::npos);  // 说清是哪个 flag
    LIFT_CHECK(bad.err.find("目录") != std::string::npos);           // 说清失败面
    // 启动前拒绝 ⇒ 一行运行摘要都不该有（不是"跑了一部分才停"）。
    LIFT_CHECK(!hasLineStartingWith(bad.out, "stop"));
    LIFT_CHECK(!hasLineStartingWith(bad.out, "dumpFailures="));

    std::printf("cli-dump-frame: created=%.1f ms blocked=%.1f ms（30 s 上限）\n", created.elapsed_ms,
                bad.elapsed_ms);

    fs::remove_all(root, ec);
#endif

    return lift_test::finish("cli-dump-frame");
}
