// emulator-lift · `--cursor-warp` 的 CLI 级守卫（`REQ-01M7BXM46NVYY3QHYW2ZKJZKZN`）
//
// 判据（红了意味着什么）：
//   ① `--cursor-warp` **缺省呈现档（headless）⇒ 宿主 exit 2**，stderr 里同时有 `--cursor-warp`
//      与 `--present headless` 的拒绝文案，且 stdout **没有任何运行摘要**（= 在读 guest 之前就死了）。
//      红了 ⇒ 一个必然无效的开关被静默接受（用户以为开了，光标一次都没动）。
//      ⛔ 不用 `WILL_FAIL TRUE`：那会让 abort（exit 3）也算绿。
//   ② 缺省档的**可判定面**：不传该 flag 时 stdout 同时有
//      `present   : … cursor-warp=off`（sink 报告的开关）与 `cursor-warp: off`（有效状态）。
//      红了 ⇒ "缺省关"变成不可判定（或有人把它默认打开：那是**用户机器的真实副作用**）。
//   ③ 开关打开且窗口可见时，有效状态行必须是 `cursor-warp: on`（不是 off / on-but-hidden）。
//      本臂用 `--present window` + **最小 PE**（guest 一条指令就停，**不会**调 SetCursorPos）
//      ⇒ 只验状态行，**不挪**任何人的光标。红了 ⇒ 开关没接上 / 接成了静默失效。
//
// ★ 为什么是 CLI 级 + 真子进程：判据穿过**参数解析 → 早期拒绝 → 启动回声 → 退出码**整条链；
//   单测某个解析函数抓不到"解析对了但 headless 闸门漏了"这种半修。
// ★ 执行源是**本用例自己构造的最小 PE32**（`nop` 一条 + `--max-insns 1`），不碰 LFS / 游戏安装。
//   子进程 cwd 指到临时目录 ⇒ 默认的 `../../corpus/...` 候选都找不到（结果不随宿主 cwd 漂）。
// ★ ②③ 会真启动呈现后端（headless / window 都要 GPU）⇒ 无 GPU 时 `LIFT_SKIP(77)`
//   （判据见 `tests/subprocess_skip.h`）；① 在 GPU 缺失之前就拒绝，任何机器都能跑。
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

using lift_test::quoteArg;

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
    // 30 s 上限：正常一次启动 + 一条指令 << 1 s；卡住必须**红**，⛔ 不许挂死流水线。
    const DWORD waited = ::WaitForSingleObject(pi.hProcess, 30000u);
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

// 最小 PE32：一节 `.text`（RVA 0x1000，入口处一条 `nop`）。`--max-insns 1` ⇒ 运行必停
//   （`stop : max-insns`），不碰任何导入 ⇒ 不需要游戏资产，也**不会**调到 SetCursorPos。
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
    w16(kPeOff + 4, 0x014Cu);                              // Machine = i386
    w16(kPeOff + 6, 1u);                                   // NumberOfSections
    w16(kPeOff + 20, static_cast<std::uint16_t>(kOptSize));  // SizeOfOptionalHeader
    w16(kPeOff + 22, 0x0102u);                             // EXECUTABLE_IMAGE | 32BIT_MACHINE
    w16(kOpt + 0, 0x010Bu);                                // Magic = PE32
    w32(kOpt + 16, 0x1000u);                               // AddressOfEntryPoint
    w32(kOpt + 28, 0x00400000u);                           // ImageBase
    w32(kOpt + 56, 0x2000u);                               // SizeOfImage
    w32(kOpt + 60, 0x200u);                                // SizeOfHeaders
    w32(kOpt + 72, 0x100000u);                             // SizeOfStackReserve
    std::memcpy(b.data() + kSec, ".text", 5);
    w32(kSec + 8, 0x1000u);   // VirtualSize
    w32(kSec + 12, 0x1000u);  // VirtualAddress
    w32(kSec + 16, 0x200u);   // SizeOfRawData
    w32(kSec + 20, 0x200u);   // PointerToRawData
    w32(kSec + 36, 0x60000020u);  // CODE | EXECUTE | READ
    b[0x200] = 0x90u;             // nop
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

std::string lineStartingWith(const std::string& text, const std::string& prefix) {
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        // ★ 子进程的 stdout 是**文件**（CRT 文本模式把 `\n` 写成 `\r\n`）⇒ 逐字比较前去掉行尾 `\r`。
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.compare(0, prefix.size(), prefix) == 0) return line;
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return "<缺 `" + prefix + "` 行>";
}

#endif  // _WIN32

}  // namespace

int main(int argc, char** argv) {
    using namespace lift_test;

#if !defined(_WIN32)
    (void)argc;
    (void)argv;
    LIFT_SKIP("--cursor-warp 的 CLI 守卫用 CreateProcessW 起子进程（Windows 专有）");
#else
    if (argc < 2) {
        LIFT_SKIP("需要 CTest 传 <emulator-lift.exe>（见 CMakeLists 的 cli-cursor-warp）");
    }
    const fs::path exe = fs::path(argv[1]);
    LIFT_CHECK(fs::exists(exe));

    std::error_code ec;
    const fs::path root = fs::temp_directory_path(ec) / ("lift-cli-cw-" + std::to_string(::GetCurrentProcessId()));
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    LIFT_CHECK(!ec);

    const fs::path fake_exe = root / "lift-minimal.exe";
    {
        const std::vector<std::uint8_t> pe = minimalPe();
        std::ofstream out(fake_exe, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(pe.data()), static_cast<std::streamsize>(pe.size()));
        out.close();
        LIFT_CHECK(fs::exists(fake_exe));
    }
    const std::wstring common = L"--exe " + quoteArg(fake_exe.wstring()) + L" --game-dir " +
                                quoteArg(root.wstring()) + L" --audio off --max-insns 1";

    // ---- ① `--cursor-warp` + 缺省呈现档（headless）⇒ exit 2（在读 guest 之前就死）----
    g_current_case = "headless-rejects-flag";
    {
        const ChildRun r =
            runChild(exe, L"--cursor-warp", root, root / "reject.out", root / "reject.err");
        LIFT_CHECK(r.started);
        LIFT_CHECK(!r.timed_out);
        LIFT_CHECK_EQ_U32(r.exit_code, 2u);
        LIFT_CHECK(r.err.find("--cursor-warp") != std::string::npos);
        LIFT_CHECK(r.err.find("--present headless") != std::string::npos);
        // 启动前拒绝 ⇒ **一行运行摘要都没有**（不是"跑了一部分才停"）
        LIFT_CHECK(!hasLineStartingWith(r.out, "stop"));
        LIFT_CHECK(!hasLineStartingWith(r.out, "insns"));
    }

    // ---- ② 缺省档（不传 flag）：stdout 两处都报 off（开关真的缺省关）----
    g_current_case = "default-off-visible";
    {
        const ChildRun r = runChild(exe, common + L" --present headless", root, root / "off.out", root / "off.err");
        LIFT_CHECK(r.started);
        LIFT_CHECK(!r.timed_out);
        LIFT_SKIP_IF_NO_GPU(r.exit_code, r.out + r.err);
        LIFT_CHECK_EQ_U32(r.exit_code, 0u);
        const std::string present = lineStartingWith(r.out, "present   :");
        LIFT_CHECK(present.find("cursor-warp=off") != std::string::npos);
        const std::string state = lineStartingWith(r.out, "cursor-warp:");
        LIFT_CHECK(state == "cursor-warp: off");
    }

    // ---- ③ 开关打开 + 可见窗口 ⇒ 有效状态行是 on（本臂不会调到 SetCursorPos）----
    g_current_case = "on-with-visible-window";
    {
        const ChildRun r =
            runChild(exe, common + L" --present window --cursor-warp", root, root / "on.out", root / "on.err");
        LIFT_CHECK(r.started);
        LIFT_CHECK(!r.timed_out);
        LIFT_SKIP_IF_NO_GPU(r.exit_code, r.out + r.err);
        LIFT_CHECK_EQ_U32(r.exit_code, 0u);
        const std::string present = lineStartingWith(r.out, "present   :");
        LIFT_CHECK(present.find("cursor-warp=on") != std::string::npos);
        const std::string state = lineStartingWith(r.out, "cursor-warp:");
        LIFT_CHECK(state == "cursor-warp: on");
    }

    fs::remove_all(root, ec);
#endif

    return lift_test::finish("cli-cursor-warp");
}
