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
//   ★ S3：`--ui` 已从 CLI 面删除（⇒ `cli-aot-flags` 的 `ui-removed-unknown-arg` 钉未知参数）。
//     旧 UiScript loader 只剩**内部** hud 回退路径（`--overlay` + 无 render 的 `--plugin`）⇒
//     本文件用 ⑨ 的那个 run 当第三个入口，与 `--overlay` / `--plugin hud` 逐字节比较。
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
// ★ S2（`--overlay` = `--plugin hud` 预设）新增判据：
//   ⑥ `--overlay --present headless` 仍 **exit 2**；`--plugin hud --present headless` **exit 0** +
//      恰一行 `render is defined but no window`（⛔ 不自动开窗）。
//   ⑦ `--overlay`（预设）/ `--plugin hud`（显式）/ **内部 hud 回退**（`--overlay` + 无 render 的
//      `--plugin`）**三方**窗口 BMP / guest dump 逐字节相同（同一帧数、只换入口 ⇒ HUD 像素 /
//      行序 / 命中键 / bold 必须不变）。
//   ⑧ `--overlay` + **显式** `--plugin`（有 render）⇒ 插件赢：像素 = 插件 fill + 恰一行取代语。
//   ⑨ `--overlay` + **无 render** 的插件 ⇒ HUD **照画**（(0,0)=#0D131A；S2 复审 I2，⛔ 静默无 HUD）。
//   ⑩ 预设不改互斥：`--overlay --input-plan` 仍 exit 5 + `input-plan: FAIL`；
//      `--overlay --plugin hud --input-plan` 仍 exit 2。
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

// ★ C2b-2：`--plugin` 的 render 夹具（`tests/plugin_fixtures/overlay-render.js`）整屏 rect 的 fill。
//   它是**不透明** `#3366CC`（alpha=FF）⇒ 两条呈现路径上都逐位等于该值（⛔ 不套混合公式）。
constexpr std::uint32_t kPluginFillRgb = 0x3366CCu;

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

    // ---- S3 删掉的用例：`--ui <语法错脚本>` ⇒ exit 2 + `<path>:<行>:<列>` -------------------
    //   ★ 为什么**删**而不是迁到 `--plugin <bad.js>`：`--ui` 的失败点在**窗口初始化头部**
    //     （GPU bring-up 之前），而 `--plugin` 的装载发生在窗口 / GPU / host 都建好之后
    //     ⇒ 同一条断言会变成"有 GPU 才可判"，缺 GPU 只能 SKIP（判据被改弱，红线）。
    //   ★ 这条 loader 级别的语法错合同**没有丢**：`ui-script-quickjs` 的
    //     `ui-script-quickjs/error-syntax` 直接对**同一个** loader 断言 `origin:line:col`
    //     （内部 hud 回退路径也走它）。

    // ---- C2b-2：`--plugin` 的 `render` 在**窗口档**真的上屏（⛔ 不需要 `--overlay`）---------
    // ★ 判据：`--present window --present-hidden --plugin <render 夹具> --max-frames 1` ⇒
    //   ① 子进程 exit 0、readout `plugin    : … render=yes … run=done`（插件真的装了、走了窗口档）；
    //   ② `--present-screenshot` 在画布原点 (0,0) 是插件 `ctx.draw.rect` 的 fill（不透明 #3366CC）；
    //   ③ 同一命令**没给** `--overlay` ⇒ adopt 自己打开叠层（hud.js 不参与），stderr 没有 `[overlay]`。
    //   红了 ⇒ aot_main 的窗口装配没把插件接进叠层 host（或又退回"只认 render 存在、不调用"）。
    //   ★ 像素期望是**不透明填充**（alpha=FF）⇒ 两条呈现路径上都逐位等于 fill，不需要混合容差。
    {
        g_current_case = "plugin-render-window";
        const fs::path plugin_js = fs::path(LIFT_PLUGIN_FIXTURES_DIR) / "overlay-render.js";
        if (!fs::exists(plugin_js)) LIFT_SKIP("夹具 tests/plugin_fixtures/overlay-render.js 不在场");
        const fs::path appdata = root / "appdata-plugin-render";
        fs::create_directories(appdata, ec);
        const fs::path shot = root / "plugin-render-shot.bmp";
        std::vector<Arg> args = common;
        args.push_back(argFromAscii("--max-frames"));
        args.push_back(argFromAscii("1"));
        args.push_back(argFromAscii("--appdata"));
        args.push_back(appdata.native());
        // ★ **不给** `--overlay`：有 `render` 的插件在窗口档由 adopt 接进叠层 host。
        args.push_back(argFromAscii("--plugin"));
        args.push_back(plugin_js.native());
        args.push_back(argFromAscii("--present-screenshot"));
        args.push_back(shot.native());
        const ChildRun r = runChild(exe, args, root, root / "plugin-render.out", root / "plugin-render.err");
        LIFT_CHECK(r.started);
        if (r.started) {
            std::printf("plugin-render-window: exit=%d timed_out=%d\n", r.exit_code, r.timed_out ? 1 : 0);
            if (r.timed_out) {
                LIFT_CHECK(false);
            } else {
                // 输入不在场 ⇒ skip（与其余 run 同一条口径；⛔ 不假红）。
                if (r.out.find("没有生成物") != std::string::npos) LIFT_SKIP("构建期没有生成物");
                if (r.err.find("没有可用的 GPU 光栅化后端") != std::string::npos) {
                    LIFT_SKIP("本机没有可用的 GPU 光栅化后端");
                }
                if (r.err.find("宿主字体不可用") != std::string::npos) LIFT_SKIP("缺宿主字体");
                if (r.err.find("拒绝启动") != std::string::npos) LIFT_SKIP("窗口 / GPU 初始化不成立");
                LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(r.exit_code), 0u);
                // ① readout：插件真的装了 + 有 render（`render=yes` 是 `hasRender()` 的读数面）。
                LIFT_CHECK(r.out.find("plugin    : ") != std::string::npos);
                LIFT_CHECK(r.out.find("render=yes") != std::string::npos);
                LIFT_CHECK(r.out.find("run=done") != std::string::npos);
                // ③ 没给 --overlay ⇒ adopt 自己开；stderr 不该有叠层失败 / hud 相关的一次性行。
                LIFT_CHECK(r.err.find("[overlay]") == std::string::npos);
                if (r.exit_code != 0 || r.out.find("plugin    : ") == std::string::npos) {
                    std::fprintf(stderr, "  stdout:\n%s\n  stderr:\n%s\n", r.out.c_str(), r.err.c_str());
                }
                // ② 窗口像素：画布原点 = 插件 fill（首帧 guest 是黑；fill 不透明 ⇒ 逐位相等）。
                const std::string shot_bytes = readAllBytes(shot);
                const BmpPixel origin = bmpPixel(shot_bytes, 0, 0);
                LIFT_CHECK(origin.ok);
                if (origin.ok) {
                    const std::uint32_t got = (static_cast<std::uint32_t>(origin.r) << 16) |
                                              (static_cast<std::uint32_t>(origin.g) << 8) |
                                              static_cast<std::uint32_t>(origin.b);
                    std::printf("plugin-render-window: shot(0,0)=#%06X want #%06X\n", got, kPluginFillRgb);
                    LIFT_CHECK_EQ_U32(got, kPluginFillRgb);
                }
            }
        }
    }

    // ---- S2：`--overlay --present headless` 仍 **exit 2**（`--overlay` 是窗口参数）----------
    //   ★ `--overlay` 现在是 "`--plugin hud` 预设"：解析先跑（一行 id 日志），窗口参数校验照旧拒绝。
    //   红了 ⇒ `--overlay` 的"要窗口"语义被预设吃掉（用户会得到一条静默的隐藏窗口 run）。
    {
        g_current_case = "overlay-headless-rejected";
        std::vector<Arg> args;
        args.push_back(argFromAscii("--exe"));
        args.push_back(age_exe.native());
        args.push_back(argFromAscii("--game-dir"));
        args.push_back(game_dir.native());
        for (const char* a : {"--skip-logo", "--present", "headless", "--time-limit", "60", "--overlay"}) {
            args.push_back(argFromAscii(a));
        }
        const ChildRun r = runChild(exe, args, root, root / "overlay-headless.out", root / "overlay-headless.err");
        LIFT_CHECK(r.started);
        std::printf("overlay-headless: exit=%d\n", r.exit_code);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(r.exit_code), 2u);
        LIFT_CHECK(r.err.find("--overlay") != std::string::npos);
    }

    // ---- S2：`--plugin hud --present headless` ⇒ **不 exit 2** + 一行 render-no-window ------
    //   ★ D-C2-5 的既有口径：render-only 插件允许 headless（`render` 永不调用）；⛔ 不自动开窗。
    //   ★ 与 `--overlay --present headless` 的对照就是 S2 的语义分界：预设**不**改变窗口需求。
    {
        g_current_case = "plugin-hud-headless";
        const fs::path appdata = root / "appdata-plugin-hud-headless";
        fs::create_directories(appdata, ec);
        std::vector<Arg> args;
        args.push_back(argFromAscii("--exe"));
        args.push_back(age_exe.native());
        args.push_back(argFromAscii("--game-dir"));
        args.push_back(game_dir.native());
        for (const char* a : {"--skip-logo", "--present", "headless", "--time-limit", "60", "--max-frames", "1"}) {
            args.push_back(argFromAscii(a));
        }
        args.push_back(argFromAscii("--appdata"));
        args.push_back(appdata.native());
        args.push_back(argFromAscii("--plugin"));
        args.push_back(argFromAscii("hud"));
        const ChildRun r = runChild(exe, args, root, root / "plugin-hud-headless.out",
                                    root / "plugin-hud-headless.err");
        LIFT_CHECK(r.started);
        std::printf("plugin-hud-headless: exit=%d\n", r.exit_code);
        if (r.out.find("没有生成物") != std::string::npos) LIFT_SKIP("构建期没有生成物");
        if (r.err.find("没有可用的 GPU 光栅化后端") != std::string::npos) LIFT_SKIP("本机没有可用的 GPU 光栅化后端");
        if (r.err.find("宿主字体不可用") != std::string::npos) LIFT_SKIP("缺宿主字体");
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(r.exit_code), 0u);
        LIFT_CHECK(r.err.find("--plugin id=hud") != std::string::npos);
        LIFT_CHECK(countOccurrences(r.err, "render is defined but no window") == 1u);
        LIFT_CHECK(r.err.find("[overlay]") == std::string::npos);  // headless 不建叠层 / 不装字体
    }

    // ---- S2：`--overlay` + **显式** `--plugin`（有 render）⇒ 插件赢 + 一次取代语（C2b-2 既有口径）----
    //   红了 ⇒ S2 的预设把"用户显式给的插件"顶掉，或者取代语被错误地当成"预设"而不打。
    {
        g_current_case = "overlay-plus-plugin-wins";
        const fs::path plugin_js = fs::path(LIFT_PLUGIN_FIXTURES_DIR) / "overlay-render.js";
        if (!fs::exists(plugin_js)) LIFT_SKIP("夹具 overlay-render.js 不在场");
        const fs::path appdata = root / "appdata-overlay-plus-plugin";
        fs::create_directories(appdata, ec);
        const fs::path shot = root / "overlay-plus-plugin.bmp";
        std::vector<Arg> args = common;
        args.push_back(argFromAscii("--max-frames"));
        args.push_back(argFromAscii("1"));
        args.push_back(argFromAscii("--appdata"));
        args.push_back(appdata.native());
        args.push_back(argFromAscii("--overlay"));
        args.push_back(argFromAscii("--plugin"));
        args.push_back(plugin_js.native());
        args.push_back(argFromAscii("--present-screenshot"));
        args.push_back(shot.native());
        const ChildRun r = runChild(exe, args, root, root / "overlay-plus-plugin.out", root / "overlay-plus-plugin.err");
        LIFT_CHECK(r.started);
        std::printf("overlay-plus-plugin: exit=%d\n", r.exit_code);
        if (r.out.find("没有生成物") != std::string::npos) LIFT_SKIP("构建期没有生成物");
        if (r.err.find("没有可用的 GPU 光栅化后端") != std::string::npos) LIFT_SKIP("本机没有可用的 GPU 光栅化后端");
        if (r.err.find("宿主字体不可用") != std::string::npos) LIFT_SKIP("缺宿主字体");
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(r.exit_code), 0u);
        LIFT_CHECK(countOccurrences(r.err, "--overlay 的 HUD 已被插件的 render 取代") == 1u);
        const std::string shot_bytes = readAllBytes(shot);
        const BmpPixel origin = bmpPixel(shot_bytes, 0, 0);
        LIFT_CHECK(origin.ok);
        if (origin.ok) {
            const std::uint32_t got = (static_cast<std::uint32_t>(origin.r) << 16) |
                                      (static_cast<std::uint32_t>(origin.g) << 8) |
                                      static_cast<std::uint32_t>(origin.b);
            std::printf("overlay-plus-plugin: shot(0,0)=#%06X want #%06X\n", got, kPluginFillRgb);
            LIFT_CHECK_EQ_U32(got, kPluginFillRgb);
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
        // ⑥ `--dump-frame` 的**新鲜度加固**（`[present-readback]` Phase A）：只靠 ③ 的"两次 dump
        //   逐字节相同"对"两次都写旧帧"失明。判据 = 每次真写盘（stderr 无 `present.cpp` 的一次性
        //   warn "帧导出失败"）+ BMP 头是**本 fixture 的帧尺寸**（1280×720）：
        //   惰性回读漏了 dump 消费者 ⇒ 这条 warn 或尺寸/非空断言先红。
        if (k.artifacts) {
            LIFT_CHECK(o.run.err.find("帧导出失败") == std::string::npos);
            const std::string dump_bytes = readAllBytes(o.dump);
            LIFT_CHECK(dump_bytes.size() > 54u);
            const std::uint32_t dump_w = le32(dump_bytes, 18);
            const std::uint32_t dump_h = le32(dump_bytes, 22);
            std::printf("⑥ [%s]: dump=%u×%u bytes=%zu stderr-无帧导出失败=%d\n", k.name, dump_w, dump_h,
                        dump_bytes.size(), o.run.err.find("帧导出失败") == std::string::npos ? 1 : 0);
            LIFT_CHECK_EQ_U32(dump_w, 1280u);
            LIFT_CHECK_EQ_U32(dump_h, 720u);
        }
        outs.push_back(std::move(o));
    }
    // ---- S2：`--plugin hud`（窗口档）与 `--overlay` **逐字节等价** ------------------------
    //   ★ 同一帧数 / 同一套参数，只把"预设"换成显式插件：窗口 BMP 必须**逐字节相同**
    //     （HUD 的行序 / 文案 / 颜色 / key / bold 全都在里面）；guest dump 也必须相同。
    //   ★ 判据可判定性由 ③/④ 的纯对照先证明（这一轮的 guest 画面是确定的）。
    ChildRun hud_run;
    fs::path hud_shot;
    fs::path hud_dump;
    {
        g_current_case = "plugin-hud-window";
        const fs::path appdata = root / "appdata-plugin-hud-window";
        fs::create_directories(appdata, ec);
        hud_shot = root / "plugin-hud-window-shot.bmp";
        hud_dump = root / "plugin-hud-window-dump.bmp";
        std::vector<Arg> args = common;
        args.push_back(argFromAscii("--max-frames"));
        args.push_back(argFromAscii("1"));
        args.push_back(argFromAscii("--appdata"));
        args.push_back(appdata.native());
        args.push_back(argFromAscii("--plugin"));
        args.push_back(argFromAscii("hud"));
        args.push_back(argFromAscii("--present-screenshot"));
        args.push_back(hud_shot.native());
        args.push_back(argFromAscii("--dump-frame"));
        args.push_back(hud_dump.native());
        hud_run = runChild(exe, args, root, root / "plugin-hud-window.out", root / "plugin-hud-window.err");
        LIFT_CHECK(hud_run.started);
        std::printf("plugin-hud-window: exit=%d\n", hud_run.exit_code);
        if (hud_run.out.find("没有生成物") != std::string::npos) LIFT_SKIP("构建期没有生成物");
        if (hud_run.err.find("没有可用的 GPU 光栅化后端") != std::string::npos) {
            LIFT_SKIP("本机没有可用的 GPU 光栅化后端");
        }
        if (hud_run.err.find("宿主字体不可用") != std::string::npos) LIFT_SKIP("缺宿主字体");
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(hud_run.exit_code), 0u);
        // 保留 id 的一次性日志（与普通 id 同形；`--overlay` 预设也走这一行）。
        LIFT_CHECK(hud_run.err.find("--plugin id=hud") != std::string::npos);
        // ⛔ 预设不是"取代"：那一行只属于显式 `--overlay` + `--plugin`。
        LIFT_CHECK(hud_run.err.find("HUD 已被插件的 render 取代") == std::string::npos);
        LIFT_CHECK(hud_run.err.find("[overlay]") == std::string::npos);
    }

    // ---- S3：旧 UiScript loader 的窗口像素由**内部 hud 回退** run 覆盖 -----------------------
    //   ★ S2 复审 I1 曾经用 `--ui <LIFT_UI_SCRIPT_DIR>/hud.js` 当"第三入口"；S3 删掉 `--ui` 之后，
    //     同一个 loader 只剩 `--overlay --plugin <无 render>` 这条产品路径（下面 ⑨）。
    //   ⇒ 不再单独跑一个 `--ui` 子进程，⑨ 的 BMP / dump 直接进 ⑦ 的三方逐字节比较。

    // ---- S2 复审 I2 / S3 第三入口：`--overlay` + **无 render** 的 `--plugin` ⇒ **HUD 照旧画** --
    //   旧行为：`wopt.overlay=overlay` ⇒ init 装默认 hud；插件没有 render ⇒ 不 adopt ⇒ hud 留着。
    //   ★ S3：这就是**内部 hud 回退**（`wopt.ui_script`）的**唯一**产品路径（`--ui` 已删）⇒
    //     本 run 的 BMP / dump 同时充当 ⑦ 的第三个入口（旧 UiScript loader）。
    //   红了 ⇒ 用户给 `--overlay` + 自动化插件时 HUD 静默消失（像素变 #000000、无任何提示）。
    fs::path fallback_shot;
    fs::path fallback_dump;
    {
        g_current_case = "overlay-with-run-only-plugin";
        const fs::path plugin_js = fs::path(LIFT_PLUGIN_FIXTURES_DIR) / "overlay-run-only.js";
        if (!fs::exists(plugin_js)) LIFT_SKIP("夹具 overlay-run-only.js 不在场");
        const fs::path appdata = root / "appdata-overlay-run-only";
        fs::create_directories(appdata, ec);
        fallback_shot = root / "overlay-run-only.bmp";
        fallback_dump = root / "overlay-run-only-dump.bmp";
        std::vector<Arg> args = common;
        args.push_back(argFromAscii("--max-frames"));
        args.push_back(argFromAscii("1"));
        args.push_back(argFromAscii("--appdata"));
        args.push_back(appdata.native());
        args.push_back(argFromAscii("--overlay"));
        args.push_back(argFromAscii("--plugin"));
        args.push_back(plugin_js.native());
        args.push_back(argFromAscii("--present-screenshot"));
        args.push_back(fallback_shot.native());
        args.push_back(argFromAscii("--dump-frame"));
        args.push_back(fallback_dump.native());
        const ChildRun r = runChild(exe, args, root, root / "overlay-run-only.out", root / "overlay-run-only.err");
        LIFT_CHECK(r.started);
        std::printf("overlay-run-only: exit=%d\n", r.exit_code);
        if (r.out.find("没有生成物") != std::string::npos) LIFT_SKIP("构建期没有生成物");
        if (r.err.find("没有可用的 GPU 光栅化后端") != std::string::npos) LIFT_SKIP("本机没有可用的 GPU 光栅化后端");
        if (r.err.find("宿主字体不可用") != std::string::npos) LIFT_SKIP("缺宿主字体");
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(r.exit_code), 0u);
        // ⛔ 无 render ⇒ 没有"取代"这回事（也不该有取代语）。
        LIFT_CHECK(r.err.find("HUD 已被插件的 render 取代") == std::string::npos);
        const BmpPixel origin = bmpPixel(readAllBytes(fallback_shot), 0, 0);
        LIFT_CHECK(origin.ok);
        if (origin.ok) {
            const std::uint32_t want = blendOverBlack(0x101820u, 204);
            std::printf("overlay-run-only: shot(0,0)=(%u,%u,%u) want #%06X\n", static_cast<unsigned>(origin.r),
                        static_cast<unsigned>(origin.g), static_cast<unsigned>(origin.b), want);
            LIFT_CHECK(nearByte(origin.r, (want >> 16) & 0xFFu, 1));
            LIFT_CHECK(nearByte(origin.g, (want >> 8) & 0xFFu, 1));
            LIFT_CHECK(nearByte(origin.b, want & 0xFFu, 1));
        }
    }

    // ---- S2 复审 I4：预设**不改**互斥语义 ------------------------------------------------
    //   ① `--overlay --input-plan <永不成立的门>` ⇒ 仍 exit 5 + `input-plan: FAIL`（预设不吞 quit /
    //      不把 quit 停掉：render-only hud 不接管 `stop_requested`）；
    //   ② `--overlay --plugin hud --input-plan` ⇒ 显式插件走**既有**互斥 ⇒ exit 2。
    {
        g_current_case = "overlay-input-plan-exit5";
        const fs::path plan =
            fs::path(LIFT_PLUGIN_FIXTURES_DIR).parent_path() / "fixtures" / "boot-title-exit-never.plan";
        if (!fs::exists(plan)) LIFT_SKIP("夹具 boot-title-exit-never.plan 不在场");
        const fs::path appdata = root / "appdata-overlay-input-plan";
        fs::create_directories(appdata, ec);
        std::vector<Arg> args = common;
        args.push_back(argFromAscii("--max-frames"));
        args.push_back(argFromAscii("1"));
        args.push_back(argFromAscii("--appdata"));
        args.push_back(appdata.native());
        args.push_back(argFromAscii("--overlay"));
        args.push_back(argFromAscii("--input-plan"));
        args.push_back(plan.native());
        const ChildRun r = runChild(exe, args, root, root / "overlay-input-plan.out", root / "overlay-input-plan.err");
        LIFT_CHECK(r.started);
        std::printf("overlay-input-plan: exit=%d\n", r.exit_code);
        if (r.out.find("没有生成物") != std::string::npos) LIFT_SKIP("构建期没有生成物");
        if (r.err.find("没有可用的 GPU 光栅化后端") != std::string::npos) LIFT_SKIP("本机没有可用的 GPU 光栅化后端");
        if (r.err.find("宿主字体不可用") != std::string::npos) LIFT_SKIP("缺宿主字体");
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(r.exit_code), 5u);
        LIFT_CHECK(r.err.find("input-plan: FAIL") != std::string::npos);
    }
    {
        g_current_case = "overlay-plugin-hud-input-plan-mutex";
        const fs::path plan =
            fs::path(LIFT_PLUGIN_FIXTURES_DIR).parent_path() / "fixtures" / "boot-title-exit-never.plan";
        std::vector<Arg> args = common;
        args.push_back(argFromAscii("--overlay"));
        args.push_back(argFromAscii("--plugin"));
        args.push_back(argFromAscii("hud"));
        args.push_back(argFromAscii("--input-plan"));
        args.push_back(plan.native());
        const ChildRun r = runChild(exe, args, root, root / "overlay-plugin-hud-plan.out",
                                    root / "overlay-plugin-hud-plan.err");
        LIFT_CHECK(r.started);
        std::printf("overlay-plugin-hud-input-plan: exit=%d\n", r.exit_code);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(r.exit_code), 2u);
        LIFT_CHECK(r.err.find("--plugin") != std::string::npos);
        LIFT_CHECK(r.err.find("--input-plan") != std::string::npos);
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

    // ⑦ S2/S3 等价（**三方**）：`--overlay`（预设）/ `--plugin hud`（显式）/
    //   **内部 hud 回退**（`--overlay` + 无 render 的 `--plugin`，旧 UiScript loader）
    //   必须产出**同一个窗口 BMP / guest dump**。
    //   ★ 判据是**逐字节**（比 hash 更直接）；可判定性由 ③/④ 的纯对照先证明。
    //   红了 ⇒ 入口不是同一条装配（HUD 行序 / 颜色 / 命中键 / bold，或 guest 像素被改写）。
    {
        g_current_case = "plugin-hud-equivalence";
        const std::string hud_shot_bytes = readAllBytes(hud_shot);
        const std::string hud_dump_bytes = readAllBytes(hud_dump);
        const std::string fallback_shot_bytes = readAllBytes(fallback_shot);
        const std::string fallback_dump_bytes = readAllBytes(fallback_dump);
        std::printf("⑦ hud-vs-overlay:  shot=%zu/%zu/%zu hud==overlay=%d fallback==overlay=%d\n",
                    hud_shot_bytes.size(), fallback_shot_bytes.size(), shot_overlay.size(),
                    hud_shot_bytes == shot_overlay ? 1 : 0, fallback_shot_bytes == shot_overlay ? 1 : 0);
        std::printf("⑦ dump-vs-overlay: dump=%zu/%zu/%zu hud==overlay=%d fallback==overlay=%d\n",
                    hud_dump_bytes.size(), fallback_dump_bytes.size(), dump_overlay.size(),
                    hud_dump_bytes == dump_overlay ? 1 : 0, fallback_dump_bytes == dump_overlay ? 1 : 0);
        LIFT_CHECK(!hud_shot_bytes.empty());
        LIFT_CHECK(!fallback_shot_bytes.empty());
        LIFT_CHECK(hud_shot_bytes == shot_overlay);
        LIFT_CHECK(fallback_shot_bytes == shot_overlay);
        LIFT_CHECK(hud_dump_bytes == dump_overlay);
        LIFT_CHECK(fallback_dump_bytes == dump_overlay);
    }

    return finish("aot-overlay");
}
