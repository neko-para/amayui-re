// emulator-lift · **用户体验锁**（`--user-run`）的写端（唯一一份）
//
// ★ 它解决什么：agent 在用户本人跑游戏体验时执行构建 / `ctest` / 变异自检（都会抢 `out/`、
//   抢 CPU）或 `kill`，会把用户打断。⇒ 用户启动时加 `--user-run`，本文件在**解析完参数后立刻**
//   写一个**机器级**锁文件 `<os temp>/amayui-re-experience.json`（两个工作树共用同一份，
//   见 AGENTS §14.5），构建 / 测试工具（`tools/experience.mjs`）据此让路。
//
// ★ 三条硬口径（⛔ 别改）：
//   ① **不改任何模拟语义**：只在宿主侧写一个文件 + 打一行，guest 可见状态逐位不变；
//   ② **不拒绝启动**：已有别的活锁 ⇒ 只 warn 并**覆盖**（用户开第二个实例是用户的事）；
//   ③ **退出必删**：RAII（`UserRunLock` 析构）+ `std::atexit` 兜底，且删之前**核对 pid**
//      （另一个实例后来覆盖了锁 ⇒ 那不是我们的锁，⛔ 不能把它的标记删掉）。
//
// ★ 为什么是 header-only：本批**不动 `CMakeLists.txt`**（另一条线刚定稿），所以锁逻辑
//   不能新增 `.cpp`。这里只含 inline 函数与一个 RAII 类，被 `aot_main.cpp` include。
//
// ★ 判活口径（与 Node 侧 `process.kill(pid, 0)` 同一条）：进程存在 ⇒ 活；已退出 ⇒ 死。
//   Windows 走 `OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)` + `GetExitCodeProcess`。
//
// ★ 失效形态（可接受，Node 侧会兜底）：`guest fault` 走 `std::_Exit(3)` ⇒ atexit 与析构都**不跑**，
//   锁会留下来；但那个 pid 已经死了 ⇒ `tools/experience.mjs status` 判 stale = INACTIVE。
#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

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
#include <csignal>
#include <unistd.h>
#endif

namespace lift::experience {

// 锁文件名（Node 侧 `tools/experience.mjs` 的 `LOCK_FILE_NAME` 必须是同一个字面量）。
inline constexpr const char* kLockFileName = "amayui-re-experience.json";

// `std::filesystem::path` → **UTF-8** 字节串。C++20 的 `u8string()` 是 `std::u8string`
// （`char8_t`），⛔ 不能直接当 `std::string` 用；`path::string()` 在 Windows 上走窄字符码页，
// 用户名里有非 ASCII 时会写坏 JSON ⇒ 两处都只能走这条转换。
inline std::string pathToUtf8(const std::filesystem::path& p) {
    const std::u8string u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

// 机器级锁路径：`<os temp>/amayui-re-experience.json`（与 `os.tmpdir()` 同一个目录）。
inline std::filesystem::path lockPath() {
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
    if (ec) return std::filesystem::path(kLockFileName);  // 极罕见：退化成 cwd 下的同名文件
    return dir / kLockFileName;
}

// 最小 JSON 字符串转义（手写最小 JSON ⇒ 只做"合法"这一件事）。
inline std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (const unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

// ISO8601 UTC（`2026-11-27T05:12:34Z`）—— Node 侧只把它当**文本**打出来 / 转 JSON。
inline std::string iso8601NowUtc() {
    const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02dZ", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

inline long long currentPid() {
#if defined(_WIN32)
    return static_cast<long long>(GetCurrentProcessId());
#else
    return static_cast<long long>(::getpid());
#endif
}

// 进程是否还活着（pid <= 0 一律当死）。
inline bool pidAlive(long long pid) {
    if (pid <= 0) return false;
#if defined(_WIN32)
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (h == nullptr) return false;
    DWORD code = 0;
    const bool ok = ::GetExitCodeProcess(h, &code) != 0;
    ::CloseHandle(h);
    return ok && code == STILL_ACTIVE;
#else
    if (::kill(static_cast<pid_t>(pid), 0) == 0) return true;
    // EPERM = 进程存在但无权发信号（⇒ 活）；ESRCH / 其它 ⇒ 死。
    return errno == EPERM;
#endif
}

// 从锁文本里取 pid（只找 `"pid"` 后面的十进制整数；⛔ 不引 JSON 解析器）。
inline bool parseLockPid(const std::string& text, long long* out) {
    const std::size_t key = text.find("\"pid\"");
    if (key == std::string::npos) return false;
    std::size_t i = text.find(':', key + 5);
    if (i == std::string::npos) return false;
    ++i;
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
    if (i >= text.size() || text[i] < '0' || text[i] > '9') return false;
    long long v = 0;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
        v = v * 10 + (text[i] - '0');
        if (v > 1000000000000LL) return false;  // 明显坏值 ⇒ 不认
        ++i;
    }
    *out = v;
    return true;
}

inline bool readLockText(std::string* text) {
    std::ifstream in(lockPath(), std::ios::binary);
    if (!in) return false;
    text->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

// 删锁——**只在盘上那份还是自己的 pid 时**才删。理由见文件头 ③：
// 另一个实例可能在本实例运行期间覆盖了锁，删掉它等于把"用户还在体验"变成静默的 INACTIVE。
// ★ 只用局部对象 / 栈上缓冲：可在 `std::atexit` 里安全调用（⛔ 不依赖任何全局对象 / 静态对象
//   的析构顺序）。
inline bool removeLockIfOwned() {
    const long long self = currentPid();
    std::string text;
    if (!readLockText(&text)) return false;  // 不存在 / 读不出来 ⇒ 没什么可删（⛔ 不瞎删）
    long long owner = 0;
    if (!parseLockPid(text, &owner)) return false;  // 认不出所有者 ⇒ 不删（宁可留个 stale 锁）
    if (owner != self) return false;                // 被别的实例覆盖了 ⇒ 不是我们的
    std::error_code ec;
    std::filesystem::remove(lockPath(), ec);
    return !ec;
}

// `std::atexit` 兜底：进程从 `main` 正常返回 / 任意返回路径都不漏（RAII 之外的第二道）。
// ★ 只调 `removeLockIfOwned()`（无状态），所以不受全局析构顺序影响。
inline void removeLockAtExit() { (void)removeLockIfOwned(); }

// RAII：`aot_main` 的栈上对象。析构时删锁；`acquire()` 成功时再登记一次 `atexit`。
class UserRunLock {
public:
    UserRunLock() = default;
    ~UserRunLock() { (void)removeLockIfOwned(); }
    UserRunLock(const UserRunLock&) = delete;
    UserRunLock& operator=(const UserRunLock&) = delete;

    // 写锁并打一行。返回是否真的落盘（失败**不致命**：用户照样能跑，只是工具看不见）。
    // `game_dir` / `appdata` 允许为空串（没给就是空串，形状与 Node 侧一致）。
    bool acquire(const std::string& exe, const std::string& cwd, const std::string& game_dir,
                 const std::string& appdata, const std::vector<std::string>& argv) {
        const long long self = currentPid();
        const std::filesystem::path path = lockPath();
        const std::string path_text = pathToUtf8(path);

        // ★ 已有别的**活**锁 ⇒ 只 warn，然后覆盖（⛔ 不拒绝启动）。
        std::string existing;
        if (readLockText(&existing)) {
            long long other = 0;
            if (parseLockPid(existing, &other) && other != self && pidAlive(other)) {
                std::fprintf(stderr, "[experience] 已有另一个体验实例 pid=%lld；本实例覆盖锁\n", other);
            }
        }

        std::string json;
        json += "{\"pid\":";
        json += std::to_string(self);
        json += ",\"started_at\":\"";
        json += jsonEscape(iso8601NowUtc());
        json += "\",\"exe\":\"";
        json += jsonEscape(exe);
        json += "\",\"cwd\":\"";
        json += jsonEscape(cwd);
        json += "\",\"game_dir\":\"";
        json += jsonEscape(game_dir);
        json += "\",\"appdata\":\"";
        json += jsonEscape(appdata);
        json += "\",\"argv\":[";
        for (std::size_t i = 0; i < argv.size(); ++i) {
            if (i != 0) json += ',';
            json += '"';
            json += jsonEscape(argv[i]);
            json += '"';
        }
        json += "]}";

        bool wrote = false;
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (out) {
                out.write(json.data(), static_cast<std::streamsize>(json.size()));
                out.flush();
                wrote = static_cast<bool>(out);
            }
        }
        if (!wrote) {
            std::fprintf(stderr, "[experience] 体验锁写不进去：%s（--user-run 继续生效，但工具看不到）\n",
                         path_text.c_str());
            return false;
        }
        // ★ 登记 `atexit` 兜底（只在成功拿到锁之后；重复登记无副作用）。
        std::atexit(&removeLockAtExit);
        std::printf("[experience] 用户正在体验（pid=%lld，lock=%s）⇒ 构建/测试工具应让路（--user-run）\n",
                    self, path_text.c_str());
        std::fflush(stdout);
        return true;
    }
};

}  // namespace lift::experience
