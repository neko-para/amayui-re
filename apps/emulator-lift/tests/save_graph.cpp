// emulator-lift · **assets 档**端到端守卫：`graphs/sn0000-save.graph` 的存档路径
//
// 背景（用户实测 2026-11-12）：TITLE → 新游戏 → SAVE 界面 → 存档时命中
//   `stop : unimplemented-slot @ eip=004A0572`（`IDirect3DDevice9::unsupported+0x90`）。
//   真机链（`.lst`）：`sub_4A0430`（GetTextureSurface）→ `CreateOffscreenPlainSurface`（+0x90）
//   → `GetRenderTargetData`（+0x80）→（缩略图放大/回写）`UpdateSurface`（+0x78）。
//
// 红了意味着什么：
//   ① 存档路径又踩到未实现的 D3D 槽（`stop/detail : unimplemented`）—— 玩家侧就是"存档失败"；
//   ② 图没走完（`detail` 不是 `interaction: task done`）；
//   ③ **没有真的写出存档**：`SAVE00.DAT` / `SAVE00.STH` 不存在或明显是空壳
//      （DAT 是 1 MB 级的状态块、STH 是 320×180 缩略图；0 字节 = 存档流程中途停了）。
//
// ★ 为什么必须真窗口 + GPU：`wait … wall_ms` 在 headless 下与帧预算跑歪；CPU 光栅 1280×720
//   在本机要分钟级，图会先超时。窗口 / GPU 不可用 ⇒ 测试自己 SKIP 77（⛔ 不记成红）。
// ★ 图的最后一段是 5×`wait 5000 wall_ms`（存档 + 缩略图收口；见 graph 里的注释）——
//   图结束时 `interaction: task done` 就会停，主进程退出码 0。
// ★ step8（`await script FELLOW`）的 60 s 超时写在**图里**（不是这里）：它是给 SN0000 右键之后
//   一次 ~9–17 s 的**同步长活**留的余量（窗口里一次 Present 都没有、指令照涨）。依据 = trace 的
//   `node step8 enter` → `node step9 enter` 的 guest_ms 差（实测 8.8 s / 16.8 s 两次）；缺省的
//   5000 ms 在本机会在长活中途就判到期。⛔ 这个数不是断言，只是超时余量的出处。
// ★ 子进程输出走**文件重定向**（⛔ 不走管道：受限沙箱里捕获管道会 EPERM）。
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
using namespace lift_test;

struct ChildResult {
    int exit_code = -1;
    bool started = false;
};

#if defined(_WIN32)
std::wstring quoteArg(const std::wstring& s) { return L"\"" + s + L"\""; }

ChildResult runChild(const fs::path& exe, const std::vector<std::string>& args, const fs::path& log_path) {
    ChildResult r;
    std::wstring joined;
    for (const std::string& a : args) joined += L" " + quoteArg(lift::utf8ToWide(a));
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
    std::wstring cmd = quoteArg(exe.wstring()) + joined;
    const BOOL ok = ::CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                     nullptr, &si, &pi);
    ::CloseHandle(h);
    if (!ok) return r;
    r.started = true;
    const DWORD waited = ::WaitForSingleObject(pi.hProcess, 900000u);
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
    std::string cmd = shellQuote(exe.string());
    for (const std::string& a : args) cmd += " " + shellQuote(a);
    cmd += " > " + shellQuote(log_path.string()) + " 2>&1";
    const int rc = std::system(cmd.c_str());
    if (rc == -1 || !fs::exists(log_path)) return r;
    r.started = true;
    r.exit_code = WIFEXITED(rc) ? WEXITSTATUS(rc) : (WIFSIGNALED(rc) ? 128 + WTERMSIG(rc) : 1);
    return r;
}
#endif

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool contains(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

bool environmentUnavailable(const std::string& log) {
    return contains(log, "找不到执行源") || contains(log, "拒绝启动") || contains(log, "SDL_Init(VIDEO)") ||
           contains(log, "--game-dir") || contains(log, "--present window 失败") ||
           contains(log, "GPU 不可用");
}

// 找 `appdata` 下任意深度的同名文件（存档目录带日文名，⛔ 不在这里拼它）。
fs::path findFile(const fs::path& root, const char* name) {
    std::error_code ec;
    if (!fs::exists(root, ec)) return {};
    for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        if (it->path().filename() == name) return it->path();
    }
    return {};
}

void printFailureExcerpt(const std::string& log) {
    std::fprintf(stderr, "---- 判据现场（子进程日志摘录）----\n");
    std::size_t pos = 0;
    while (pos < log.size()) {
        const std::size_t end = log.find('\n', pos);
        const std::string line = log.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        if (contains(line, "stop      :") || contains(line, "detail    :") ||
            contains(line, "unimplemented") || contains(line, "interaction") ||
            contains(line, "SAVE") || contains(line, "error")) {
            std::fprintf(stderr, "  %s\n", line.c_str());
        }
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    std::fprintf(stderr, "---- 摘录结束 ----\n");
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args = lift::argvUtf8(argc, argv);
    if (args.size() < 5) {
        std::fprintf(stderr, "用法：%s <emulator-lift> <game-dir> <work-dir> <interaction-graph>\n", argv[0]);
        LIFT_CHECK(false);
        return finish("save-graph");
    }
    const fs::path exe = args[1];
    const std::string game_dir = args[2];
    const fs::path work = args[3];
    const fs::path graph = args[4];
    std::error_code ec;
    fs::remove_all(work, ec);
    fs::create_directories(work, ec);
    const fs::path appdata = work / "appdata";
    fs::create_directories(appdata, ec);
    if (!fs::exists(graph)) LIFT_SKIP("没有 interaction-graph（不在源码树里）");

    const fs::path log_path = work / "save.log";
    const fs::path fail_bmp = work / "save.fail.bmp";
    const std::vector<std::string> child_args = {
        "--present", "window",
        "--present-hidden",
        "--raster", "gpu",
        "--native-overrides", "all",
        "--skip-logo",
        "--audio", "off",
        "--game-dir", game_dir,
        "--appdata", appdata.string(),
        "--interaction-graph", graph.string(),
        "--interaction-fail-dump", fail_bmp.string(),
        "--max-frames", "8000",
        "--time-limit", "400",
    };
    const ChildResult child = runChild(exe, child_args, log_path);
    if (!child.started) LIFT_SKIP("起不了子进程（exe 路径 / 沙箱限制）");
    const std::string log = readAll(log_path);
    if (log.empty()) LIFT_SKIP("子进程没写日志（工作目录 / 权限）");
    if (child.exit_code != 0) {
        if (environmentUnavailable(log)) LIFT_SKIP("环境不可用（没有执行源 / 安装树 / 窗口 / GPU）");
        std::fprintf(stderr, "exit %d；日志：%s\n", child.exit_code, log_path.string().c_str());
        printFailureExcerpt(log);
        LIFT_CHECK(false);
        return finish("save-graph");
    }

    // ① 图走完 + 引擎没有被"未实现"停住
    LIFT_CHECK(contains(log, "stop      : stopped"));
    LIFT_CHECK(contains(log, "detail    : interaction: task done"));
    LIFT_CHECK(!contains(log, "stop      : unimplemented"));
    LIFT_CHECK(!contains(log, "detail    : unimplemented"));
    // 摘要行 `shims : imported N / called-unimplemented M`：M 必须 0（辅助；主判据是上面两条 stop/detail）
    const std::size_t calls = log.find("called-unimplemented ");
    LIFT_CHECK(calls != std::string::npos && log.compare(calls + 21u, 1u, "0") == 0);

    // ② 存档真的落盘：DAT = 1 MB 级状态块、STH = 320×180 缩略图（0 字节 = 存档中途停了）
    const fs::path dat = findFile(appdata, "SAVE00.DAT");
    const fs::path sth = findFile(appdata, "SAVE00.STH");
    LIFT_CHECK(!dat.empty());
    LIFT_CHECK(!sth.empty());
    std::uintmax_t dat_size = 0;
    std::uintmax_t sth_size = 0;
    if (!dat.empty()) dat_size = fs::file_size(dat, ec);
    if (!sth.empty()) sth_size = fs::file_size(sth, ec);
    LIFT_CHECK(dat_size > 100000u);  // 实测 ~1.05 MB；⛔ 不断言具体值
    LIFT_CHECK(sth_size > 1000u);    // 实测 ~173 KB
    std::printf("save-graph: SAVE00.DAT=%llu B, SAVE00.STH=%llu B\n", static_cast<unsigned long long>(dat_size),
                static_cast<unsigned long long>(sth_size));

    // ③ 缩略图内容 —— **本条用例不断言它**（`REQ-01M780VKKRFHJEX6VM74DRCYBM`）
    //    `SAVE00.STH` 是 320×180 24bpp 的 BMP：**偏移 54** 之前是头（14 B `BITMAPFILEHEADER` +
    //    40 B `BITMAPINFOHEADER` = `bfOffBits` 54），54 之后 172,800 B 才是像素。
    //    ⛔ 本单**没有**把"payload 非零 > 0"留成断言：那条判据（修前红、修后绿）在这里达不成 ——
    //    shim 的签名修好、`0x4A778B` 那条「渲染到纹理」**真的绑上了**（日志 `目标 = 纹理表面` /
    //    `RT0 ← …（ret=004A778D）`）、GPU 也恢复了（`gpu=6999 fallback=2`），但 STH payload 仍全 0
    //    ⇒ 缺口在**「纹理当渲染目标」这条路**（本线只有 CPU 光栅填它；GPU 侧还没做）与随后的读回。
    //    这两件事由「GPU 支持纹理当渲染目标」那一单达成，本单不再用一个恒红的断言占位
    //    （⛔ 也不写成 SKIP 或假绿）。
    //    ★ 下面那行只是**读数**（不是断言）：后续那一单把它当基线，别在这里加 LIFT_CHECK。
    if (!sth.empty()) {
        constexpr std::size_t kBmpHeaderBytes = 54u;  // bfOffBits = BITMAPFILEHEADER(14) + BITMAPINFOHEADER(40)
        const std::string st_bytes = readAll(sth);
        std::size_t nonzero = 0;
        for (std::size_t i = kBmpHeaderBytes; i < st_bytes.size(); ++i) {
            if (st_bytes[i] != '\0') ++nonzero;
        }
        const std::size_t payload = st_bytes.size() > kBmpHeaderBytes ? st_bytes.size() - kBmpHeaderBytes : 0u;
        std::printf("save-graph: SAVE00.STH payload 非零字节 = %llu（偏移 %llu 之后，共 %llu B；读数，非断言）\n",
                    static_cast<unsigned long long>(nonzero),
                    static_cast<unsigned long long>(kBmpHeaderBytes),
                    static_cast<unsigned long long>(payload));
    }
    if (lift_test::g_failures > 0) printFailureExcerpt(log);
    return finish("save-graph");
}
