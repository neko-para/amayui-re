#include "runtime/host_encoding.h"

#include <climits>
#include <cstdint>
#include <cstdio>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace lift {
namespace {

#if !defined(_WIN32)
// UTF-8 编码一个码点（只接受合法范围；代理项由调用方合并成完整码点）。
// ★ Windows 上走 WideCharToMultiByte（见 wideToUtf8），这段只给 POSIX 的 wideToUtf8 用。
void appendUtf8(std::string& out, std::uint32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFF) {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}
#endif

// 严格 UTF-8 解码：过短编码 / 代理项 / > U+10FFFF / 截断 都算失败（返回 false）。
bool decodeUtf8(const std::string& s, std::vector<std::uint32_t>* out) {
    out->clear();
    out->reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        const auto b0 = static_cast<unsigned char>(s[i]);
        std::uint32_t cp = 0;
        std::size_t extra = 0;
        if (b0 < 0x80) {
            cp = b0;
        } else if ((b0 & 0xE0) == 0xC0) {
            cp = b0 & 0x1Fu;
            extra = 1;
            if (cp < 2) return false;  // overlong
        } else if ((b0 & 0xF0) == 0xE0) {
            cp = b0 & 0x0Fu;
            extra = 2;
        } else if ((b0 & 0xF8) == 0xF0) {
            cp = b0 & 0x07u;
            extra = 3;
        } else {
            return false;
        }
        if (extra != 0 && i + extra >= s.size()) return false;
        for (std::size_t k = 1; k <= extra; ++k) {
            const auto bk = static_cast<unsigned char>(s[i + k]);
            if ((bk & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (bk & 0x3Fu);
        }
        // 过短编码
        if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) || (extra == 3 && cp < 0x10000)) return false;
        if (cp >= 0xD800 && cp <= 0xDFFF) return false;
        if (cp > 0x10FFFF) return false;
        out->push_back(cp);
        i += extra + 1;
    }
    return true;
}

#if defined(_WIN32)
std::string wideToUtf8Impl(const wchar_t* w, int len) {
    if (!w) return {};
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
    if (need <= 0) return {};
    std::string out(static_cast<std::size_t>(need), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, len, out.data(), need, nullptr, nullptr);
    // len == -1 时 WideCharToMultiByte 把结尾 NUL 也写进来；对外契约是"普通字符串"，去掉它。
    if (len < 0 && !out.empty() && out.back() == '\0') out.pop_back();
    return out;
}

// 控制台码页的 RAII 守卫：正常退出时恢复；`_Exit` / abort 路径由 restoreConsoleUtf8() 显式调用。
struct ConsoleCpGuard {
    UINT old_output = 0;
    bool changed = false;
    void restore() {
        if (!changed) return;
        // ★ 先 flush：析构发生在 CRT 最后一次 flush 之前；不先 flush 会把缓冲里的半行
        //   用**恢复后的**码页写出去。
        std::fflush(stdout);
        std::fflush(stderr);
        ::SetConsoleOutputCP(old_output);
        changed = false;
    }
    ~ConsoleCpGuard() { restore(); }
};

ConsoleCpGuard& consoleCpGuard() {
    static ConsoleCpGuard guard;
    return guard;
}
#endif

}  // namespace

std::string wideToUtf8(const wchar_t* w) {
    if (!w) return {};
#if defined(_WIN32)
    return wideToUtf8Impl(w, -1);
#else
    std::string out;
    for (std::size_t i = 0; w[i] != 0; ++i) {
        std::uint32_t cp = static_cast<std::uint32_t>(w[i]);
#if WCHAR_MAX <= 0xFFFF
        // 16 位 wchar_t 的平台：合并代理对
        if (cp >= 0xD800 && cp <= 0xDBFF && w[i + 1] != 0) {
            const std::uint32_t lo = static_cast<std::uint32_t>(w[i + 1]);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                ++i;
            }
        }
#endif
        appendUtf8(out, cp);
    }
    return out;
#endif
}
std::wstring utf8ToWide(const std::string& s) {
    std::vector<std::uint32_t> cps;
    if (!decodeUtf8(s, &cps)) return {};
    std::wstring out;
    out.reserve(cps.size());
    for (const std::uint32_t cp : cps) {
#if defined(_WIN32)
        if (cp <= 0xFFFF) {
            out.push_back(static_cast<wchar_t>(cp));
        } else {
            const std::uint32_t v = cp - 0x10000;
            out.push_back(static_cast<wchar_t>(0xD800 + (v >> 10)));
            out.push_back(static_cast<wchar_t>(0xDC00 + (v & 0x3FF)));
        }
#else
        out.push_back(static_cast<wchar_t>(cp));
#endif
    }
    return out;
}

std::vector<std::string> argvUtf8(int argc, char** argv) {
    std::vector<std::string> out;
#if defined(_WIN32)
    struct LocalFreeGuard {
        LPWSTR* p = nullptr;
        ~LocalFreeGuard() {
            if (p) ::LocalFree(p);
        }
    };
    int wargc = 0;
    LocalFreeGuard guard{::CommandLineToArgvW(::GetCommandLineW(), &wargc)};
    if (guard.p && wargc > 0) {
        out.reserve(static_cast<std::size_t>(wargc));
        for (int i = 0; i < wargc; ++i) out.push_back(wideToUtf8Impl(guard.p[i], -1));
        return out;
    }
    // ⛔ 不静默退回 ANSI argv：那正是本缺陷（非 ASCII 路径按 936 落成 GBK 字节）。
    std::fprintf(stderr, "[host-encoding] CommandLineToArgvW 失败 ⇒ 回退 CRT argv（非 ASCII 路径可能乱码）\n");
#endif
    out.reserve(static_cast<std::size_t>(argc > 0 ? argc : 0));
    for (int i = 0; i < argc; ++i) out.emplace_back(argv[i] ? argv[i] : "");
    return out;
}

void initConsoleUtf8() {
#if defined(_WIN32)
    // ★ 判据用 `GetConsoleOutputCP()` 而不是 `GetConsoleMode(stdout)`：控制台句柄可能只有
    //   写权限（`GetConsoleMode` 会因缺 GENERIC_READ 失败），而码页是**整个控制台**的属性。
    //   非 0 = 进程挂着一个控制台；0 = 没有（纯重定向 / 服务）⇒ 不动字节。
    if (::GetConsoleOutputCP() == 0) return;
    ConsoleCpGuard& guard = consoleCpGuard();
    if (guard.changed) return;
    guard.old_output = ::GetConsoleOutputCP();
    if (::SetConsoleOutputCP(CP_UTF8) != 0) guard.changed = true;
#else
    (void)0;
#endif
}

void restoreConsoleUtf8() {
#if defined(_WIN32)
    consoleCpGuard().restore();
#endif
}

}  // namespace lift
