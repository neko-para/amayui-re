// emulator-lift · **AOT 臂**的 `--sound` / `--audio` / `--audio-dump` **接线**（CLI 级 + 真子进程 + 真 run）
//
// 判据（红了意味着什么）—— 全部读**引擎自己打出来的读数**，⛔ 无一读"手写的说明文本"：
//   ① 只给 `--sound`（不给 `--audio` / `--audio-dump`）⇒ 收口摘要里**没有** `audio     :` 一行，
//      也没有 `audio-pump:` 一行（档位 = off ⇒ `AudioEngine::enabled()` 为假 ⇒ 那两行不打）。
//      红了 ⇒ `present_flags.h` 的 `kAotSoundNotice` 又会指着一行**不存在**的东西
//      （洞 4：它曾写"读数：收口时 stdout 的 audio 一行"，而只给 `--sound` 时那一行不存在）。
//   ② `--sound --audio virtual` ⇒ 有那一行，且 `muted=0`。★ `muted=` 是 `AudioEngine::dumpSummary`
//      从 `mute_` 打出来的**引擎状态**，不是通知文本里的字面量。
//   ③ 对照组 `--audio virtual`（**不给** `--sound`）⇒ 同一行 `muted=1`。
//      ★ ②③ 合起来才是"`--sound` 真的接进静音门"的机械判据：删掉 `aot_main` 里
//      `configureAudio(…, sound, …)` 的 `sound`（或 `AotHost::configureAudio` 里把它写死）
//      ⇒ ② 变成 `muted=1` ⇒ **这条用例红**。
//      洞 6：旧的 `cli-aot-flags`/`sound-accepted-with-once-notice` 只断言**自己打的那句文案**
//      里含 `muted=0`（自证式断言）⇒ 接线坏了它照绿。本文件取代那条作为证据。
//   ④ `--sound --audio-dump <path>` ⇒ 同一行 `muted=0` + **WAV 文件真的存在**：
//      `size >= 44`、头部两个长度字段与文件大小自洽（`RIFF size == size-8`、`data size == size-44`）。
//      ★ 洞 5：说明文本说 dump 是"44 字节头（data=0）"——S3 之后它取决于这一跑**有没有真交付**，
//        所以这里只断言**结构**（头自洽 + 至少 44 B）+ 把实测字节数打出来（读数，不作断言）。
//      ★ 洞 7：`std::abort()` 路径跳过 `AudioEngine::end()` ⇒ 两个长度字段**不回填** ⇒
//        那种 run 的 dump 不可信。本用例的前提是"正常收口"（见下面的 `stop      :` 断言）：
//        abort 的 run 连摘要都没有，会先在①那条前提上红。
//
// ★ 为什么必须**真 run**（而不是纯 `AudioEngine` 夹具）：`--sound` → `AotHost::configureAudio()`
//   → `AudioEngine::configure(…, sound, …)` 这三段是**接线**，纯夹具只能钉住最后一段
//   （`aot-audio-flags` 的 ③ 已经钉了 `AudioEngine` 自己）。洞 6 的判据正是"删接线 ⇒ 红"。
// ★ 代价：一次 run ≈ 0.5 s（`--max-frames 5`；本机实测 4 次 run 合计 ≈ 2 s，CTest 读数 2.1 s）。
// ★ 注册在**顶层无条件**（`SKIP_RETURN_CODE 77`）—— 缺 GPU / 缺生成物 / 缺执行源时 `LIFT_SKIP(77)`，
//   ⛔ 不许"什么都不做然后 return 0"（那与跑绿了长得一样，`check.h`）。
// ★ 子进程 cwd 指到临时目录、`--appdata` 也指到临时目录 ⇒ 不往仓库 / 真 AppData 写东西。
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
    // 60 s 上限：`--max-frames 5` 在本机 1–2 s（有安装树时含 skip-logo）；卡住必须**红**。
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

/** `audio     :` 那一行（**收口摘要**；⚠ `audio-pump:` 也是 `audio` 开头 ⇒ 必须再认 `mode=`）。 */
std::string audioSummaryLine(const std::string& out) {
    std::size_t pos = 0;
    while (pos <= out.size()) {
        const std::size_t nl = out.find('\n', pos);
        const std::string line = out.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (line.compare(0, 5, "audio") == 0 && line.find("mode=") != std::string::npos) return line;
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return {};
}

std::uint32_t readLe32(const std::string& b, std::size_t off) {
    if (b.size() < off + 4) return 0xFFFFFFFFu;
    return static_cast<std::uint32_t>(static_cast<unsigned char>(b[off])) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b[off + 1])) << 8) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b[off + 2])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b[off + 3])) << 24);
}

// 一次 run 的公共 argv（`--exe` 由调用方拼在前面；`--appdata` 每次一个全新空目录）。
// ★ `--max-frames 5`：判据只要"跑到收口"，5 帧足够（本机 1–2 s）。
const wchar_t* const kCommonArgs = L" --max-frames 5 --skip-logo --present headless --time-limit 60";

#endif  // _WIN32

}  // namespace

int main(int argc, char** argv) {
    using namespace lift_test;

#if !defined(_WIN32)
    (void)argc;
    (void)argv;
    LIFT_SKIP("AOT 臂的接线用 CreateProcessW 起子进程（Windows 专有）");
#else
    if (argc < 3) {
        LIFT_SKIP("需要 CTest 传 <emulator-lift-aot.exe> <AGE.EXE__dumped.sectfix.EXE>（见 CMakeLists）");
    }
    const fs::path exe = fs::path(argv[1]);
    const fs::path age_exe = fs::path(argv[2]);
    if (!fs::exists(exe)) LIFT_SKIP("emulator-lift-aot.exe 不在场");
    // 缺 LFS 资产 ⇒ **skip**（不是"跑绿"）：没有执行源就没有 run，也就没有接线可判。
    if (!fs::exists(age_exe)) LIFT_SKIP("执行源（corpus/assets/age/AGE.EXE__dumped.sectfix.EXE）不在场");

    std::error_code ec;
    const fs::path root = fs::temp_directory_path(ec) / ("lift-aot-audio-wiring-" + std::to_string(::GetCurrentProcessId()));
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    LIFT_CHECK(!ec);

    const std::wstring exe_arg = L"--exe " + quoteArg(age_exe.wstring());

    struct Run {
        const char* name;
        std::wstring extra;  // 追加在公共 argv 之后
        bool want_audio_line;
        int want_muted;      // 只在 want_audio_line 时有意义（0 / 1）
        bool want_dump;
    };
    // ★ 四条 = 3 个档位 + 1 个对照组。`--audio-dump` 会把档位提升成 virtual（唯一规则在
    //   `AudioEngine::configure`，`aot-audio-flags` 已钉），所以它不另给 `--audio`。
    const fs::path dump_path = root / "wiring-dump.wav";
    const Run kRuns[] = {
        {"sound-only", L"--sound", /*want_audio_line=*/false, 0, false},
        {"sound-virtual", L"--sound --audio virtual", true, 0, false},
        {"virtual-no-sound", L"--audio virtual", true, 1, false},
        {"sound-dump", L"--sound --audio-dump " + quoteArg(dump_path.wstring()), true, 0, true},
    };

    for (const Run& k : kRuns) {
        g_current_case = k.name;
        const fs::path appdata = root / (std::string("appdata-") + k.name);
        fs::create_directories(appdata, ec);
        const std::wstring args = exe_arg + kCommonArgs + L" --appdata " +
                                  quoteArg(appdata.wstring()) + L" " + k.extra;
        const std::string tag = std::string(k.name) + ".";
        const ChildRun r = runChild(exe, args, root, root / (tag + "out"), root / (tag + "err"));
        LIFT_CHECK(r.started);
        if (!r.started) continue;
        LIFT_CHECK(!r.timed_out);
        if (r.timed_out) continue;
        // 前置事实（不是判据的替代品）：run 必须**跑到收口摘要**。
        //   ⇒ 没有生成物 / 没有 GPU / 缺执行源这类"输入不在场"在这里**跳过**（⛔ 不当红）。
        if (r.out.find("没有生成物") != std::string::npos) LIFT_SKIP("构建期没有生成物（缺 clang / 执行源 / closure）");
        if (r.err.find("没有可用的 GPU 光栅化后端") != std::string::npos) LIFT_SKIP("本机没有可用的 GPU 光栅化后端");
        const std::string stop = firstLineStartingWith(r.out, "stop      : ");
        if (stop.empty()) {
            LIFT_CHECK(false);  // 说明上面的 skip 条件没覆盖这一种 ⇒ 红并让 output-on-failure 带出全文
            continue;
        }

        const std::string audio = audioSummaryLine(r.out);
        const std::string pump = firstLineStartingWith(r.out, "audio-pump:");
        if (!k.want_audio_line) {
            // ①（洞 4）：只给 `--sound` ⇒ 档位 off ⇒ 两行都**不存在**。
            if (!audio.empty())
                lift_test::failf(__FILE__, __LINE__,
                                 "只给 --sound 时 stdout 不该有 `audio     :` 一行（档位是 off）");
            if (!pump.empty())
                lift_test::failf(__FILE__, __LINE__, "只给 --sound 时 stdout 不该有 `audio-pump:` 一行");
            continue;
        }
        LIFT_CHECK(!audio.empty());
        if (audio.empty()) continue;
        // ②③（洞 6）：`muted=` 是引擎状态（`AudioEngine::dumpSummary` 读 `mute_`）。
        char want[32];
        std::snprintf(want, sizeof(want), "muted=%d ", k.want_muted);
        if (audio.find(want) == std::string::npos)
            lift_test::failf(__FILE__, __LINE__, "`%s` 里没有 `%s` ⇒ --sound 没有真的进静音门（全文：%s）",
                             k.name, want, audio.c_str());
        if (k.want_dump) {
            // ④（洞 5/7）：WAV 在场 + 头自洽（正常收口由上面的 `stop      :` 保证）。
            const std::string bytes = readAllBytes(dump_path);
            const std::uint64_t size = bytes.size();
            // ★ 读数（不是断言）：实测字节数打在 ctest 日志里。
            std::printf("aot-audio-wiring: dump=%s bytes=%llu\n", k.name,
                        static_cast<unsigned long long>(size));
            LIFT_CHECK(size >= 44);
            if (size >= 44) {
                const std::uint32_t riff = readLe32(bytes, 4);
                const std::uint32_t data = readLe32(bytes, 40);
                if (riff != size - 8u)
                    lift_test::failf(__FILE__, __LINE__, "RIFF size=%u != size-8=%llu（头没回填 ⇒ dump 不可信）",
                                     riff, static_cast<unsigned long long>(size - 8u));
                if (data != size - 44u)
                    lift_test::failf(__FILE__, __LINE__, "data size=%u != size-44=%llu（头没回填 ⇒ dump 不可信）",
                                     data, static_cast<unsigned long long>(size - 44u));
            }
        }
    }

    fs::remove_all(root, ec);
#endif

    return lift_test::finish("aot-audio-wiring");
}
