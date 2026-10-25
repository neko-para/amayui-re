#include "core/encoding.h"

#include <string>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#elif defined(__has_include)
#if __has_include(<iconv.h>)
#include <iconv.h>
#define EMULATOR_CXX_HAVE_SYSTEM_ICONV 1
#endif
#endif

namespace core {
namespace {

#ifdef _WIN32
std::wstring cp932_to_wide(const std::string& bytes) {
    if (bytes.empty()) return {};
    int length = MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, bytes.data(),
                                     static_cast<int>(bytes.size()), nullptr, 0);
    if (length <= 0) {
        length = MultiByteToWideChar(932, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    }
    if (length <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(932, 0, bytes.data(), static_cast<int>(bytes.size()), wide.data(), length);
    return wide;
}

std::string wide_to_utf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string utf8(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), length,
                        nullptr, nullptr);
    return utf8;
}
#endif

}  // namespace

std::string cp932_to_utf8(const std::string& bytes) {
#ifdef _WIN32
    return wide_to_utf8(cp932_to_wide(bytes));
#elif defined(EMULATOR_CXX_HAVE_SYSTEM_ICONV)
    if (bytes.empty()) return {};
    const char* names[] = {"CP932", "SHIFT_JIS", "SJIS"};
    for (const char* name : names) {
        iconv_t cd = iconv_open("UTF-8", name);
        if (cd == reinterpret_cast<iconv_t>(-1)) continue;
        std::vector<char> output(bytes.size() * 4 + 16, 0);
        char* in_ptr = const_cast<char*>(bytes.data());
        std::size_t in_left = bytes.size();
        char* out_ptr = output.data();
        std::size_t out_left = output.size();
        const std::size_t result = iconv(cd, &in_ptr, &in_left, &out_ptr, &out_left);
        iconv_close(cd);
        if (result == static_cast<std::size_t>(-1)) continue;
        return std::string(output.data(), output.size() - out_left);
    }
    return bytes;  // 没有 iconv：原样返回（只影响显示，不影响字节层）
#else
    return bytes;
#endif
}

std::filesystem::path path_from_utf8(const std::string& utf8) {
    return std::filesystem::path(std::u8string(
        reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::filesystem::path path_from_engine(const std::string& cp932) {
    std::string utf8 = cp932_to_utf8(cp932);
#ifndef _WIN32
    // 引擎路径用反斜杠；POSIX 上它不是分隔符，统一规范成 `/`。
    for (char& ch : utf8) {
        if (ch == '\\') ch = '/';
    }
#endif
    return path_from_utf8(utf8);
}

std::string path_to_utf8(const std::filesystem::path& path) {
#ifdef _WIN32
    const std::u8string utf8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
#else
    return path.string();
#endif
}

}  // namespace core
