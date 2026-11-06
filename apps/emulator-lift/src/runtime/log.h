// emulator-lift · 统一日志分级 / 分类 / 模式
//
// ★ 为什么不是"每条 shim 都打一行"：引擎的正常探测（`CreateFileA("TITLE00.PNG")` 失败 ⇒
//   它随后从 ALF 取）在旧实现里看起来像错误；一跑几百帧就淹掉真正的现场。
// ★ 口径：
//   · **Error / Warn 是"人要看"的**；Info 是正常事件；Debug 是探测 / 细节；Trace 是逐调用。
//   · 默认 `warn`：只显示 Warn + Error；`--verbose` = info；`--log-level debug|trace` 看细节。
//   · **Error 永远显示**（即使 `quiet`）；`shimAbort` / trap 不走本闸门。
//   · 期望内的文件探测失败用 `Debug`（`pathFailure(..., LogLevel::Debug)`），默认不可见。
//   · 分类过滤（`--log-cat file,com`）只影响 < Error 的行；Error 永远显示。
#pragma once

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace lift {

enum class LogLevel : int {
    Quiet = 0,  // 只显示 Error（硬失败）
    Error = 1,
    Warn = 2,
    Info = 3,
    Debug = 4,
    Trace = 5,
};

enum LogCat : std::uint32_t {
    LogGeneral = 1u << 0,
    LogShim = 1u << 1,
    LogFile = 1u << 2,
    LogVfs = 1u << 3,
    LogCom = 1u << 4,
    LogD3d9 = 1u << 5,
    LogD3dx9 = 1u << 6,
    LogDsound = 1u << 7,
    LogWinmm = 1u << 8,
    LogVideo = 1u << 9,
    LogLoader = 1u << 10,
    LogNative = 1u << 11,
    LogReport = 1u << 12,
    LogAll = 0xFFFFFFFFu,
};

struct LogConfig {
    LogLevel level = LogLevel::Warn;
    std::uint32_t categories = LogAll;
    bool timestamp = false;  // 保留位：默认关（确定性输出）
};

inline LogConfig& logConfig() {
    static LogConfig cfg;
    return cfg;
}

inline const char* logLevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Quiet: return "quiet";
        case LogLevel::Error: return "error";
        case LogLevel::Warn: return "warn";
        case LogLevel::Info: return "info";
        case LogLevel::Debug: return "debug";
        case LogLevel::Trace: return "trace";
    }
    return "?";
}

inline bool logParseLevel(const char* text, LogLevel* out) {
    if (!text || !out) return false;
    if (std::strcmp(text, "quiet") == 0) *out = LogLevel::Quiet;
    else if (std::strcmp(text, "error") == 0) *out = LogLevel::Error;
    else if (std::strcmp(text, "warn") == 0) *out = LogLevel::Warn;
    else if (std::strcmp(text, "info") == 0) *out = LogLevel::Info;
    else if (std::strcmp(text, "debug") == 0) *out = LogLevel::Debug;
    else if (std::strcmp(text, "trace") == 0) *out = LogLevel::Trace;
    else return false;
    return true;
}

inline bool logSetLevel(const char* text) {
    LogLevel level = LogLevel::Warn;
    if (!logParseLevel(text, &level)) return false;
    logConfig().level = level;
    return true;
}

// `--log-cat file,com,vfs`；`all` / 空 = 全开。未知名字 ⇒ false（调用方报错，不静默）。
inline bool logSetCategories(const char* csv) {
    if (!csv || !*csv) {
        logConfig().categories = LogAll;
        return true;
    }
    std::uint32_t mask = 0;
    bool saw = false;
    const char* p = csv;
    while (*p) {
        while (*p == ',' || *p == ' ' || *p == '\t') ++p;
        const char* begin = p;
        while (*p && *p != ',' && *p != ' ' && *p != '\t') ++p;
        const std::size_t n = static_cast<std::size_t>(p - begin);
        if (n == 0) break;
        auto eq = [begin, n](const char* name) {
            return std::strlen(name) == n && std::strncmp(begin, name, n) == 0;
        };
        if (eq("all")) { mask = LogAll; saw = true; }
        else if (eq("none")) { mask = 0; saw = true; }
        else if (eq("general")) { mask |= LogGeneral; saw = true; }
        else if (eq("shim")) { mask |= LogShim; saw = true; }
        else if (eq("file")) { mask |= LogFile; saw = true; }
        else if (eq("vfs")) { mask |= LogVfs; saw = true; }
        else if (eq("com")) { mask |= LogCom; saw = true; }
        else if (eq("d3d9")) { mask |= LogD3d9; saw = true; }
        else if (eq("d3dx9")) { mask |= LogD3dx9; saw = true; }
        else if (eq("dsound")) { mask |= LogDsound; saw = true; }
        else if (eq("winmm")) { mask |= LogWinmm; saw = true; }
        else if (eq("video")) { mask |= LogVideo; saw = true; }
        else if (eq("loader")) { mask |= LogLoader; saw = true; }
        else if (eq("native")) { mask |= LogNative; saw = true; }
        else if (eq("report")) { mask |= LogReport; saw = true; }
        else {
            std::fprintf(stderr, "[log] 未知分类 \"%.*s\"（可用：all,none,general,shim,file,vfs,com,d3d9,d3dx9,dsound,winmm,video,loader,native,report）\n",
                         static_cast<int>(n), begin);
            return false;
        }
    }
    logConfig().categories = saw ? mask : LogAll;
    return true;
}

inline bool logVisible(LogLevel level, std::uint32_t category) {
    // 硬失败不受级别 / 分类闸门影响：⛔ 不能因为 quiet 把 unimplemented 隐藏掉。
    if (level == LogLevel::Error) return true;
    if (static_cast<int>(level) > static_cast<int>(logConfig().level)) return false;
    return (logConfig().categories & category) != 0;
}

// 统一的落点：prefix 形如 `[shim] kernel32!CreateFileA: `（由各 Log helper 拼好）。
// Info 不额外加级别标签（保持既有日志格式）；Warn/Debug/Trace 前加一行级别标签，便于 grep。
inline void logV(LogLevel level, std::uint32_t category, const char* prefix, const char* fmt, va_list ap) {
    if (!logVisible(level, category)) return;
    switch (level) {
        case LogLevel::Warn: std::fputs("[warn] ", stderr); break;
        case LogLevel::Debug: std::fputs("[debug] ", stderr); break;
        case LogLevel::Trace: std::fputs("[trace] ", stderr); break;
        case LogLevel::Error: std::fputs("[error] ", stderr); break;
        case LogLevel::Info: break;
        case LogLevel::Quiet: break;
    }
    if (prefix && *prefix) std::fputs(prefix, stderr);
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

inline void logAt(LogLevel level, std::uint32_t category, const char* prefix, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    logV(level, category, prefix, fmt, ap);
    va_end(ap);
}

}  // namespace lift
