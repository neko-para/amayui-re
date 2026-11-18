// emulator-lift · **AOT 臂**的 `--overlay` 端到端（真子进程 + 真窗口像素）
//
// 判据（红了意味着什么）：
//   ① `--present window --present-hidden --overlay --max-frames 2` ⇒ **exit 0**（M0 判据 1）。
//      红了 ⇒ AOT 臂的 `--overlay` 参数面 / 接线没通（用户给了参数却跑不起来）。
//   ② `--present-screenshot` 的窗口 BMP 在**画布原点**（1280×720 帧 / 窗口 ⇒ (0,0)）是
//      HUD 底色 `#101820CC` 与底层画面混合后的值：直出（D 单起走 `SRC_ALPHA / ONE_MINUS_SRC_ALPHA`
//      的混合管线）与 renderer（`SDL_BLENDMODE_BLEND`）**都是** 80% 混合 ⇒ 首帧 guest 是黑 ⇒
//      `blend(#101820, #000000, 204)` = `(13,19,26)`（两条路径同一个期望，±1 舍入）。
//      同一条命令**不带** `--overlay` 时那个像素是 guest 后缓冲的颜色（首帧 = 黑）。
//      红了 ⇒ 叠层没有真的上屏，或原点 / 半透明度错了。
//   ★ 另：`--ui <path>` 的**语法错** ⇒ 子进程 exit 2 + stderr 点名 `<path>:<行>:<列>`。
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
// ★ 脚手架**跨平台**（Windows `CreateProcessW` / POSIX `fork+execv`，同一 `runChild` 签名）：
//   参数构造只有一份（`Arg = fs::path::string_type`：win = 宽字符，POSIX = 字节），⛔ 不拼 shell
//   字符串、⛔ 不用 `system("timeout …")`（路径含空格 / 引号会错）。非「资产 / GPU 缺口」不再 SKIP。
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
#else
#include <cerrno>
#include <chrono>
#include <csignal>
#include <ctime>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;

// ★ 参数类型 = `fs::path::string_type`：Windows 上是 `std::wstring`（`CreateProcessW` + `cmdline.h`
//   的 MSVC 引用规则），POSIX 上是 `std::string`（`execv` 直接吃字节；本仓路径口径是 UTF-8）。
//   ⇒ 两个后端共用**同一条参数构造路径**，⛔ 不各写一套转义 / 拼串。
using Arg = fs::path::string_type;

/** ASCII 字面量 ⇒ `Arg`（两个平台都只走 ASCII；路径一律用 `fs::path::native()`）。 */
Arg argFromAscii(const char* s) {
    Arg out;
    for (const char* p = s; *p != '\0'; ++p) out.push_back(static_cast<typename Arg::value_type>(*p));
    return out;
}

struct ChildRun {
    int exit_code = -1;
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

#if defined(_WIN32)

ChildRun runChild(const fs::path& exe, const std::vector<Arg>& args, const fs::path& cwd, const fs::path& out_path,
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
    // ★ 命令行拼装只有一份实现（`tests/cmdline.h`）：结尾反斜杠 / 内嵌引号 / 空串都按 MSVC 规则转义。
    std::vector<std::wstring> argv;
    argv.reserve(args.size() + 1);
    argv.push_back(exe.native());
    argv.insert(argv.end(), args.begin(), args.end());
    std::wstring cmd = lift_test::joinCommandLine(argv);
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
    DWORD code = 0xFFFFFFFFu;
    ::GetExitCodeProcess(pi.hProcess, &code);
    r.exit_code = static_cast<int>(code);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    r.out = readAllBytes(out_path);
    r.err = readAllBytes(err_path);
    return r;
}

#else  // POSIX

// 子进程里只做 async-signal-safe 的事；这几条系统调用都可能被信号打断 ⇒ EINTR 重试
// （⛔ 不把 EINTR 当失败：那会让一次信号变成 `_exit(127)` 的假红）。
int openRetry(const char* path, int flags) {
    int fd = -1;
    do {
        fd = ::open(path, flags, 0600);
    } while (fd < 0 && errno == EINTR);
    return fd;
}

int dup2Retry(int from, int to) {
    int rc = -1;
    do {
        rc = ::dup2(from, to);
    } while (rc < 0 && errno == EINTR);
    return rc;
}

int chdirRetry(const char* path) {
    int rc = -1;
    do {
        rc = ::chdir(path);
    } while (rc < 0 && errno == EINTR);
    return rc;
}

ChildRun runChild(const fs::path& exe, const std::vector<Arg>& args, const fs::path& cwd, const fs::path& out_path,
                  const fs::path& err_path) {
    ChildRun r;
    // argv 在**父进程**里先建好（fork 之后子进程不再分配内存；指针在 fork 后仍有效）。
    std::vector<char*> argv;
    argv.reserve(args.size() + 2);
    argv.push_back(const_cast<char*>(exe.c_str()));
    for (const Arg& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    const pid_t pid = ::fork();
    if (pid < 0) return r;
    if (pid == 0) {
        // ---- 子进程 ----
        // ★ 只做 async-signal-safe 的事；任何失败一律 `_exit(127)`（父进程按"非 0 退出码"判红）。
        // ★ stdout/stderr 是**普通文件**（⛔ 不是管道）⇒ 这条路径不产生 SIGPIPE；也⛔ 不在这里
        //   `signal(SIGPIPE, SIG_IGN)`（exec 后仍被继承 ⇒ 会悄悄改掉产品自己的 SIGPIPE 处理）。
        const int fd_out = openRetry(out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC);
        const int fd_err = openRetry(err_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC);
        if (fd_out < 0 || fd_err < 0) ::_exit(127);
        if (dup2Retry(fd_out, STDOUT_FILENO) < 0) ::_exit(127);
        if (dup2Retry(fd_err, STDERR_FILENO) < 0) ::_exit(127);
        if (chdirRetry(cwd.c_str()) != 0) ::_exit(127);
        // 不把父进程的其它 fd 带进 exec：3 以上全关（含 fd_out/fd_err 的原始副本；它们已 dup2 到 1/2）。
        // ★ 上界夹到 65536：Linux 的 `_SC_OPEN_MAX` 可能是 1M，而本测试进程不可能有那么高的 fd。
        const long open_max = ::sysconf(_SC_OPEN_MAX);
        const int close_upto = (open_max > 0 && open_max < 65536) ? static_cast<int>(open_max) : 65536;
        for (int fd = 3; fd < close_upto; ++fd) ::close(fd);
        for (;;) {  // execv 只在**失败**时返回；EINTR 也是失败的一种 ⇒ 重试
            ::execv(exe.c_str(), argv.data());
            if (errno != EINTR) break;
        }
        ::_exit(127);  // exec 失败（exe 不存在 / 不可执行 / ENOEXEC）
    }
    // ---- 父进程 ----
    r.started = true;
    int status = 0;
    bool reaped = false;
    bool timed_out = false;
    // 60 s 上限，与 Windows 分支一致；`WNOHANG` 轮询 + `std::chrono` 计时。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    for (;;) {
        const pid_t w = ::waitpid(pid, &status, WNOHANG);
        if (w == pid) {
            reaped = true;
            break;
        }
        if (w < 0) {
            if (errno == EINTR) continue;  // 被信号打断 ⇒ 重试（⛔ 不许把 EINTR 当"子进程没了"）
            break;                         // 其它错误：拿不到状态 ⇒ 下面记 -1（非 0）
        }
        // w == 0：还在跑。
        if (std::chrono::steady_clock::now() >= deadline) {
            timed_out = true;
            break;
        }
        // 轮询间隔 10 ms（`nanosleep` 被信号打断也**不用补睡**：循环回头会重算 deadline）。
        // ★ 不用 `std::this_thread::sleep_for`：POSIX 上它可能把 libstdc++ 的线程符号拖进链接行，
        //   而这条测试只需要一个纯 POSIX 的睡眠原语。
        const struct timespec poll = {0, 10 * 1000 * 1000};
        ::nanosleep(&poll, nullptr);
    }
    if (timed_out) {
        r.timed_out = true;
        ::kill(pid, SIGKILL);
        for (;;) {  // 收尸同样要过 EINTR
            const pid_t w = ::waitpid(pid, &status, 0);
            if (w == pid) break;
            if (w < 0 && errno == EINTR) continue;
            break;
        }
        r.exit_code = 991;  // 与 Windows 分支的 `TerminateProcess(…, 991)` 对齐
    } else if (reaped) {
        if (WIFEXITED(status)) {
            r.exit_code = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            r.exit_code = 128 + WTERMSIG(status);  // 异常终止 ⇒ 非 0
        } else {
            r.exit_code = -1;
        }
    } else {
        r.exit_code = -1;  // waitpid 非 EINTR 失败（如 ECHILD）⇒ 非 0（⛔ 不当成 0）
    }
    r.out = readAllBytes(out_path);
    r.err = readAllBytes(err_path);
    return r;
}

#endif  // _WIN32

/**
 * 从收口摘要 `present   : … lastHash=0x…` 里取 `lastHash=0x…` 这一段（没有 ⇒ 空串）。
 * ★ 启动 readout 也打 `present   : driver=…`（**没有** `lastHash=`，`aot_main.cpp` 的 `describe()`）⇒
 *   必须按 `lastHash=` 认那一条，⛔ 不能只按前缀取第一行（那会永远取到启动行 ⇒ 本判据假红）。
 */
std::string lastHashField(const std::string& out) {
    const std::string prefix = "present   : ";
    std::size_t pos = 0;
    while (pos <= out.size()) {
        const std::size_t nl = out.find('\n', pos);
        const std::string line = out.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (line.compare(0, prefix.size(), prefix) == 0) {
            const std::size_t at = line.find("lastHash=");
            if (at != std::string::npos) {
                const std::size_t end = line.find(' ', at);
                return line.substr(at, end == std::string::npos ? std::string::npos : end - at);
            }
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return {};
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

// ★ HUD 的底是 `#101820CC`（alpha 204 ⇒ 80% 不透明）。**两条呈现路径都与底下的画面混合**
//   （renderer = `SDL_BLENDMODE_BLEND`；直出 = D 单的 `SRC_ALPHA / ONE_MINUS_SRC_ALPHA` 混合管线）⇒
//   首帧 guest 是黑 ⇒ 期望 = `(s*a + 127)/255`（整数、四舍五入）= `#0D131A` = (13,19,26)。
constexpr std::uint32_t blendOverBlack(std::uint32_t src_rgb, int a) {
    const std::uint32_t r = ((src_rgb >> 16) & 0xFFu) * static_cast<std::uint32_t>(a);
    const std::uint32_t g = ((src_rgb >> 8) & 0xFFu) * static_cast<std::uint32_t>(a);
    const std::uint32_t b = (src_rgb & 0xFFu) * static_cast<std::uint32_t>(a);
    return (((r + 127u) / 255u) << 16) | (((g + 127u) / 255u) << 8) | ((b + 127u) / 255u);
}

// 逐通道 `|got - want| <= tol`（混合侧给 1 的容差：GPU / 软件 blend 的舍入）。
bool nearByte(std::uint8_t got, std::uint32_t want, int tol) {
    const int d = static_cast<int>(got) - static_cast<int>(want);
    return d <= tol && -d <= tol;
}

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

}  // namespace

int main(int argc, char** argv) {
    using namespace lift_test;

    if (argc < 4) {
        LIFT_SKIP("需要 CTest 传 <emulator-lift-aot> <AGE.EXE__dumped.sectfix.EXE> <安装树>（见 CMakeLists）");
    }
    const fs::path exe = fs::path(argv[1]);
    const fs::path age_exe = fs::path(argv[2]);
    const fs::path game_dir = fs::path(argv[3]);
    if (!fs::exists(exe)) LIFT_SKIP("emulator-lift-aot 不在场");
    if (!fs::exists(age_exe)) LIFT_SKIP("执行源（corpus/assets/age/AGE.EXE__dumped.sectfix.EXE）不在场");
    if (!fs::exists(game_dir)) LIFT_SKIP("安装树（--game-dir）不在场：没有 AGERC.DLL 就跑不到 Present");

    std::error_code ec;
#if defined(_WIN32)
    const unsigned long pid = ::GetCurrentProcessId();
#else
    const pid_t pid = ::getpid();
#endif
    const fs::path root = fs::temp_directory_path(ec) / ("lift-aot-overlay-" + std::to_string(pid));
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    LIFT_CHECK(!ec);

    // 公共参数（两个平台共用一份构造；`Arg` 就是子进程真正收到的 argv token）。
    std::vector<Arg> common;
    common.push_back(argFromAscii("--exe"));
    common.push_back(age_exe.native());
    common.push_back(argFromAscii("--game-dir"));
    common.push_back(game_dir.native());
    for (const char* a : {"--skip-logo", "--present", "window", "--present-hidden", "--time-limit", "60"}) {
        common.push_back(argFromAscii(a));
    }

    // ---- `--ui <path>` 的语法错 ⇒ **exit 2** + stderr 点名 `<path>:<行>:<列>`（B）--------
    // ★ 判据是**启动硬错**：脚本编译失败必须让 AOT 臂拒绝启动（exit 2），且错误里带文件名与行号
    //   （用户据此改自己的脚本）。输入是自造的：`bad.js` 第 2 行少一个值 ⇒ 语法错落在第 2 行。
    // ★ 这一步在 GPU bring-up **之前**就返回（脚本加载在 `SdlWindow::init` 的头部）⇒ 不依赖 GPU；
    //   但**依赖宿主字体**（字体缺失 ⇒ 先在字体上失败）⇒ 那一条按 SKIP 处理（⛔ 不假红）。
    {
        g_current_case = "ui-syntax-error";
        const fs::path bad = root / "bad.js";
        {
            std::ofstream out(bad, std::ios::binary | std::ios::trunc);
            out << "// 第 1 行：注释（让语法错落在第 2 行）\n"
                   "ui.rect({ x: 0, y: });\n";
        }
        const fs::path appdata = root / "appdata-ui-syntax";
        fs::create_directories(appdata, ec);
        std::vector<Arg> args = common;
        args.push_back(argFromAscii("--appdata"));
        args.push_back(appdata.native());
        args.push_back(argFromAscii("--ui"));
        args.push_back(bad.native());
        const ChildRun r = runChild(exe, args, root, root / "ui-syntax.out", root / "ui-syntax.err");
        LIFT_CHECK(r.started);
        std::printf("--ui 语法错：exit=%d\n", r.exit_code);
        if (r.err.find("宿主字体不可用") != std::string::npos) LIFT_SKIP("缺宿主字体（LIFT_HOST_FONT_PATH）");
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(r.exit_code), 2u);
        // ★ 只锁"文件 + 行"（`bad.js:2:`）；列号交给 QuickJS 的版本，⛔ 不在这里钉死。
        LIFT_CHECK(r.err.find("bad.js:2:") != std::string::npos);
        if (r.err.find("bad.js:2:") == std::string::npos) {
            std::fprintf(stderr, "  stderr:\n%s\n", r.err.c_str());
        }
    }

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
        std::vector<Arg> args = common;
        args.push_back(argFromAscii("--max-frames"));
        args.push_back(argFromAscii(std::to_string(k.frames).c_str()));
        args.push_back(argFromAscii("--appdata"));
        args.push_back(appdata.native());
        Out o;
        o.shot = root / (std::string(k.name) + "-shot.bmp");
        o.dump = root / (std::string(k.name) + "-dump.bmp");
        if (k.artifacts) {
            args.push_back(argFromAscii("--present-screenshot"));
            args.push_back(o.shot.native());
            args.push_back(argFromAscii("--dump-frame"));
            args.push_back(o.dump.native());
        }
        if (k.overlay) args.push_back(argFromAscii("--overlay"));
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
            std::fprintf(stderr, "  [%s] exit=%d\n  stderr:\n%s\n", k.name, o.run.exit_code, o.run.err.c_str());
        }
        // ⑤ 无周期行：`[overlay]` / `[ui]` 各至多一条（一次性 readout 允许）。
        // ★ 读数（不是额外断言）：让 `ctest -V` 直接看到 ① 的 exit 与 ⑤ 的计数。
        const std::size_t overlay_lines = countOccurrences(o.run.err, "[overlay]");
        const std::size_t ui_lines = countOccurrences(o.run.err, "[ui]");
        std::printf("①/⑤ [%s]: exit=%d [overlay]=%zu [ui]=%zu\n", k.name, o.run.exit_code, overlay_lines,
                    ui_lines);
        LIFT_CHECK(overlay_lines <= 1);
        LIFT_CHECK(ui_lines <= 1);
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
    std::printf("② shot(0,0): overlay=(%u,%u,%u) plain=(%u,%u,%u)\n", static_cast<unsigned>(with_overlay.r),
                static_cast<unsigned>(with_overlay.g), static_cast<unsigned>(with_overlay.b),
                static_cast<unsigned>(without_overlay.r), static_cast<unsigned>(without_overlay.g),
                static_cast<unsigned>(without_overlay.b));
    // ★ 期望的底色**两条路径相同**（子进程自己在启动 readout 里报走的是哪条：
    //   `present   : … present=direct/<mode>`，见 `SdlWindow::describe`）：
    //     · 直出：D 单起走 `SRC_ALPHA / ONE_MINUS_SRC_ALPHA` 混合管线 ⇒ 与首帧 guest（黑）混合；
    //     · renderer：`SDL_BLENDMODE_BLEND` ⇒ 同样与首帧 guest（黑）混合。
    //   ⇒ 都是 `#101820CC` 的 80% = `blend(#101820, #000000, 204)`，±1 舍入。
    const bool direct = outs[1].run.out.find("present=direct") != std::string::npos;
    const std::uint32_t want_rgb = blendOverBlack(0x101820u, 204);
    const int tol = 1;
    std::printf("② 呈现路径：%s ⇒ 期望底色 #%06X（tol=%d）\n", direct ? "direct（80% 混合）" : "renderer（80% 混合）",
                want_rgb, tol);
    LIFT_CHECK(nearByte(with_overlay.r, (want_rgb >> 16) & 0xFFu, tol));
    LIFT_CHECK(nearByte(with_overlay.g, (want_rgb >> 8) & 0xFFu, tol));
    LIFT_CHECK(nearByte(with_overlay.b, want_rgb & 0xFFu, tol));
    LIFT_CHECK(with_overlay.b != without_overlay.b || with_overlay.g != without_overlay.g ||
               with_overlay.r != without_overlay.r);

    // ③ guest 帧逐字节相同（⛔ UI 不进 guest 后缓冲）。
    const std::string dump_overlay = readAllBytes(outs[1].dump);
    const std::string dump_plain = readAllBytes(outs[2].dump);
    std::printf("③ dump: overlay=%zu bytes plain=%zu bytes equal=%d\n", dump_overlay.size(), dump_plain.size(),
                dump_overlay == dump_plain ? 1 : 0);
    LIFT_CHECK(!dump_overlay.empty());
    LIFT_CHECK(dump_overlay == dump_plain);

    // ④ arena.hash（最后一次 Present 的后缓冲哈希）两次相同。
    const std::string hash_overlay = lastHashField(outs[1].run.out);
    const std::string hash_plain = lastHashField(outs[2].run.out);
    std::printf("④ lastHash: overlay=%s plain=%s\n", hash_overlay.c_str(), hash_plain.c_str());
    LIFT_CHECK(!hash_overlay.empty());
    LIFT_CHECK(hash_overlay == hash_plain);

    return finish("aot-overlay");
}
