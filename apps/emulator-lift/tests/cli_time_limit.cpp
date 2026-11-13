// emulator-lift · `--time-limit` 的 CLI 级守卫（`REQ-01M711JN018QDSWK4726Y1A0KV`）
//
// 判据（红了意味着什么）：
//   ① **`--time-limit 0` 与省略该参数完全等价**：同一条命令加/不加它，退出码、运行摘要里的
//      `stop` / `insns` / `blocks` / `trace-in` / `instr` 五行**逐字相同**，且 `stop` 不是
//      `time-limit`（即 `0` 真的被当成"不设墙钟上限"，既不拒绝、也不是"立即到点"）。
//   ② **正数仍然生效**（本用例用 `600`）：它不该被解析成"0 / 不限"或"非法"—— 否则"删掉这个
//      flag 的解析"也能让 ① 变绿。
//   ③ `--time-limit -1` 与 `--time-limit abc` **仍然 exit 2**，stderr 里有 `--time-limit`
//      （用户得知道是哪个 flag 错了），且 stdout 里**没有** `stop` 行 ⇒ 是启动前就拒绝，
//      不是"跑了一半才停"。
//
// ★ 为什么是 CLI 级 + 真子进程：缺陷的形态是"usage 说 0 = 不限，代码却 exit 2"——
//   判据必须穿过**参数解析 → 运行 → 摘要 → 退出码**这条完整链路，单测某个解析函数抓不到
//   "解析对了但运行时把 0 当成极小上限"这种半修。
// ★ 为什么在纯档（顶层无条件注册）里能跑真实运行：执行源是**本用例自己构造的最小 PE32**
//   （`nop` 一条 + `--max-insns 1` ⇒ `stop: max-insns`），不碰 LFS / 游戏安装
//   （与 `tests/conformance.cpp` 的自造机器码同一口径）。子进程的 cwd 指到一个临时目录，
//   这样默认的 `../../corpus/...` 执行源 / `.lst` 候选都找不到 —— ⛔ 不要让它按宿主 cwd 漂。
//
// ★ 红过的形状（修前实测，见需求单）：`--time-limit 0` ⇒ exit 2 + `--time-limit 需要正数秒`，
//   摘要一行都没有；省略该参数 ⇒ exit 0 + `stop : max-insns`。
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

// `text` 里以 `prefix` 开头的那一整行；没有 ⇒ 返回 "<缺 …>"（失败时能直接印出来定位）
std::string lineStartingWith(const std::string& text, const std::string& prefix) {
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t nl = text.find('\n', pos);
        const std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (line.compare(0, prefix.size(), prefix) == 0) return line;
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return "<缺 `" + prefix + "` 行>";
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

// 本用例的参数全是 ASCII（`-1` / `abc` / 路径）⇒ 逐字节放宽即可。
std::wstring widen(const std::string& s) {
    std::wstring w;
    w.reserve(s.size());
    for (const char c : s) w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    return w;
}

#endif  // _WIN32

}  // namespace

int main(int argc, char** argv) {
    using namespace lift_test;

#if !defined(_WIN32)
    (void)argc;
    (void)argv;
    LIFT_SKIP("--time-limit 的 CLI 守卫用 CreateProcessW 起子进程（Windows 专有）");
#else
    if (argc < 2) {
        LIFT_SKIP("需要 CTest 传 <emulator-lift.exe>（见 CMakeLists 的 cli-time-limit）");
    }
    const fs::path exe = fs::path(argv[1]);
    LIFT_CHECK(fs::exists(exe));

    std::error_code ec;
    const fs::path root =
        fs::temp_directory_path(ec) / ("lift-cli-tl-" + std::to_string(::GetCurrentProcessId()));
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
    //   ⇒ 运行**必停**在一条指令之后；`--time-limit` 的读数正好落在摘要里可比。
    const std::wstring common = L"--exe " + quoteArg(fake_exe.wstring()) + L" --game-dir " +
                                quoteArg(root.wstring()) + L" --audio off --max-insns 1";

    auto run = [&](const char* tag, const std::wstring& extra) {
        const fs::path o = root / (std::string(tag) + ".out");
        const fs::path e = root / (std::string(tag) + ".err");
        const std::wstring args = common + L" " + extra;
        return runChild(exe, args, root, o, e);
    };

    // ---- ① 省略 --time-limit（基准）----
    g_current_case = "omit";
    const ChildRun omit = run("omit", L"");
    LIFT_CHECK(omit.started);
    LIFT_CHECK(!omit.timed_out);
    // ★ 无 GPU（含 `SDL_VIDEODRIVER=dummy`）：子进程在跑 guest 之前就拒绝启动 ⇒ SKIP 77。
    //   真正的参数错误 / 回归仍然红（判据见 `tests/subprocess_skip.h`）。
    LIFT_SKIP_IF_NO_GPU(omit.exit_code, omit.err);
    LIFT_CHECK_EQ_U32(omit.exit_code, 0u);
    LIFT_CHECK(hasLineStartingWith(omit.out, "stop"));
    LIFT_CHECK(hasLineStartingWith(omit.out, "insns"));
    LIFT_CHECK(lineStartingWith(omit.out, "stop").find("max-insns") != std::string::npos);
    // ★ `present-tail` 只在**本轮 ≥1 次 Present**时打；这条 run 是自造 PE + `--max-insns 1` ⇒ **0 次 Present**
    //   ⇒ 摘要里**不许**有这行（把"没有 Present"打成 `tail_insns=0` 会被读成"没卡"）。
    LIFT_CHECK(!hasLineStartingWith(omit.out, "present-tail"));

    // ---- ① `--time-limit 0` 必须与省略**完全等价** ----
    g_current_case = "zero-equals-omit";
    const ChildRun zero = run("zero", L"--time-limit 0");
    LIFT_CHECK(zero.started);
    LIFT_CHECK(!zero.timed_out);
    LIFT_CHECK_EQ_U32(zero.exit_code, omit.exit_code);
    // 这五行 = "同一条命令的观测面"（停止原因 / 指令数 / 块缓存 / 输入清单 / 仪器位）。
    // ⛔ 不做全量 stdout 比对：那会把**无关的输出行**也钉死（那是脆弱守卫）。
    for (const char* prefix : {"trace-in", "instr", "stop", "insns", "blocks"}) {
        const std::string a = lineStartingWith(omit.out, prefix);
        const std::string b = lineStartingWith(zero.out, prefix);
        if (a != b) {
            std::fprintf(stderr, "  省略 --time-limit : %s\n  --time-limit 0    : %s\n", a.c_str(), b.c_str());
        }
        LIFT_CHECK(a == b);
    }
    // 反向断言：`0` 不是"立即到点"（那会变成 `stop : time-limit` + `detail : 达到 --time-limit…`）
    //   ★ 只查这两行，不查整份 stdout：路径里可能天然含 "time-limit" 字样（脆弱守卫）。
    LIFT_CHECK(lineStartingWith(zero.out, "stop").find("time-limit") == std::string::npos);
    LIFT_CHECK(!hasLineStartingWith(zero.out, "detail"));

    // ---- ② 正数仍然生效（不是"一律当不限"）----
    g_current_case = "positive-still-accepted";
    const ChildRun pos = run("pos", L"--time-limit 600");
    LIFT_CHECK(pos.started);
    LIFT_CHECK(!pos.timed_out);
    LIFT_CHECK_EQ_U32(pos.exit_code, 0u);
    LIFT_CHECK(lineStartingWith(pos.out, "stop") == lineStartingWith(omit.out, "stop"));
    LIFT_CHECK(lineStartingWith(pos.out, "insns") == lineStartingWith(omit.out, "insns"));

    // ---- ③ 负数 / 非数字仍然 exit 2，且消息说清是哪个 flag ----
    for (const char* bad : {"-1", "abc"}) {
        const std::string tag = std::string("bad-") + bad;
        g_current_case = tag.c_str();
        const ChildRun r = run(tag.c_str(), L"--time-limit " + widen(bad));
        LIFT_CHECK(r.started);
        LIFT_CHECK(!r.timed_out);
        LIFT_CHECK_EQ_U32(r.exit_code, 2u);
        LIFT_CHECK(r.err.find("--time-limit") != std::string::npos);
        // 启动前拒绝 ⇒ 一行运行摘要都不该有（不是"跑了一部分才停"）
        LIFT_CHECK(!hasLineStartingWith(r.out, "stop"));
    }

    fs::remove_all(root, ec);
#endif

    return lift_test::finish("cli-time-limit");
}
