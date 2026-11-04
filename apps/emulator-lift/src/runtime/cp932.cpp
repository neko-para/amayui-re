// emulator-lift · CP932 ↔ UTF-16 实现（表见派生文件 cp932_table.h）
#include "runtime/cp932.h"

#include <algorithm>
#include <unordered_map>

#include "runtime/cp932_table.h"

namespace lift {
namespace {

using cp932_table::kPairs;
using cp932_table::kSingle;

constexpr std::size_t kPairCount = sizeof(kPairs) / sizeof(kPairs[0]);

struct ReverseTable {
    std::unordered_map<char16_t, std::uint16_t> map;  // code unit → (lead<<8)|trail（单字节 trail=0）
    ReverseTable() {
        map.reserve(kPairCount + 256);
        // 单字节：ASCII / 半角片假名 / 8 个例外（0x00..0x7F 与 0xA1..0xDF 之外也有定义）
        for (int b = 0; b < 256; ++b) {
            const std::uint16_t u = kSingle[b];
            if (u == 0xFFFFu) continue;
            map.emplace(static_cast<char16_t>(u), static_cast<std::uint16_t>(b));
        }
        // ★ Windows 的 CP932 编码器把 U+00A5（¥）与 U+203E（‾）映射到 0x5C / 0x7E。
        //   解码方向 0x5C 仍是 U+005C（与 CP932 一致）⇒ 这是**有意的单向别名**。
        map.emplace(static_cast<char16_t>(0x00A5), static_cast<std::uint16_t>(0x5C));
        map.emplace(static_cast<char16_t>(0x203E), static_cast<std::uint16_t>(0x7E));
        for (std::size_t i = 0; i < kPairCount; ++i) {
            const std::uint32_t v = kPairs[i];
            const auto unit = static_cast<char16_t>(v & 0xFFFFu);
            const auto both = static_cast<std::uint16_t>(((v >> 24) << 8) | ((v >> 16) & 0xFFu));
            map.emplace(unit, both);  // 重复映射保留表中第一条（表已按 (lead,trail) 升序）
        }
    }
};

const ReverseTable& reverse() {
    static const ReverseTable r;  // 只读、构造确定 ⇒ 多线程只读也安全（本线 guest 单线程）
    return r;
}

}  // namespace

int cp932Decode(const std::uint8_t* p, std::size_t n, char16_t* out) {
    if (!p || n == 0) return 0;
    const std::uint8_t b0 = p[0];
    // 单字节：先查表（覆盖 ASCII / 半角片假名 / 8 个例外）
    if (b0 < 0x80 || (b0 >= 0xA1 && b0 <= 0xDF) || b0 == 0x80 || b0 == 0xA0 || (b0 >= 0xFD)) {
        const std::uint16_t u = kSingle[b0];
        if (u == 0xFFFFu) return 0;
        if (out) *out = static_cast<char16_t>(u);
        return 1;
    }
    const bool lead = (b0 >= 0x81 && b0 <= 0x9F) || (b0 >= 0xE0 && b0 <= 0xFC);
    if (!lead || n < 2) return 0;
    const std::uint8_t b1 = p[1];
    const std::uint32_t key = (static_cast<std::uint32_t>(b0) << 24) | (static_cast<std::uint32_t>(b1) << 16);
    auto it = std::lower_bound(kPairs, kPairs + kPairCount, key);
    if (it == kPairs + kPairCount) return 0;
    if ((*it & 0xFFFF0000u) != key) return 0;
    if (out) *out = static_cast<char16_t>(*it & 0xFFFFu);
    return 2;
}

int cp932Encode(char16_t w, std::uint8_t out[2]) {
    const auto it = reverse().map.find(w);
    if (it == reverse().map.end()) return 0;
    const std::uint16_t both = it->second;
    if ((both >> 8) == 0) {
        if (out) out[0] = static_cast<std::uint8_t>(both & 0xFF);
        return 1;
    }
    if (out) {
        out[0] = static_cast<std::uint8_t>((both >> 8) & 0xFF);
        out[1] = static_cast<std::uint8_t>(both & 0xFF);
    }
    return 2;
}

std::u16string cp932ToWide(std::string_view bytes, std::size_t* bad) {
    std::u16string out;
    out.reserve(bytes.size());
    const auto* p = reinterpret_cast<const std::uint8_t*>(bytes.data());
    std::size_t i = 0;
    while (i < bytes.size()) {
        char16_t u = 0;
        const int n = cp932Decode(p + i, bytes.size() - i, &u);
        if (n == 0) {
            if (bad) ++*bad;
            ++i;  // Windows 的 MultiByteToWideChar 遇非法序列也会推进
            continue;
        }
        out.push_back(u);
        i += static_cast<std::size_t>(n);
    }
    return out;
}

std::string wideToCp932(std::u16string_view text, std::size_t* bad) {
    std::string out;
    out.reserve(text.size());
    for (const char16_t w : text) {
        std::uint8_t b[2] = {};
        const int n = cp932Encode(w, b);
        if (n == 0) {
            if (bad) ++*bad;
            out.push_back('?');
            continue;
        }
        out.append(reinterpret_cast<const char*>(b), static_cast<std::size_t>(n));
    }
    return out;
}

// ---------------------------------------------------------------------------------------
// UTF-8 ↔ UTF-16（实现与 kernel32_file.cpp 原先的局部版本逐行相同；搬到这里是为了让
// W 版 API 的 shim 不必各写一份）
// ---------------------------------------------------------------------------------------
std::u16string utf8ToUtf16(std::string_view s, std::size_t* bad, char16_t replacement) {
    std::u16string out;
    out.reserve(s.size());
    std::size_t bad_count = 0;
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::uint32_t cp = 0;
        std::size_t len = 0;
        if (c < 0x80) {
            cp = c;
            len = 1;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1Fu;
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0Fu;
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07u;
            len = 4;
        } else {
            ++bad_count;
            out.push_back(replacement);
            ++i;
            continue;
        }
        if (i + len > s.size()) {
            ++bad_count;
            out.push_back(replacement);
            ++i;
            continue;
        }
        bool ok = true;
        for (std::size_t k = 1; k < len; ++k) {
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        const std::uint32_t min_cp = (len == 1) ? 0u : (len == 2) ? 0x80u : (len == 3) ? 0x800u : 0x10000u;
        if (!ok || cp < min_cp || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) {
            ++bad_count;
            out.push_back(replacement);
            ++i;
            continue;
        }
        i += len;
        if (cp <= 0xFFFFu) {
            out.push_back(static_cast<char16_t>(cp));
        } else {
            cp -= 0x10000u;
            out.push_back(static_cast<char16_t>(0xD800u + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00u + (cp & 0x3FFu)));
        }
    }
    if (bad) *bad = bad_count;
    return out;
}

std::string utf16ToUtf8(std::u16string_view w, std::size_t* bad, char32_t replacement) {
    std::string out;
    out.reserve(w.size());
    std::size_t bad_count = 0;
    for (std::size_t i = 0; i < w.size(); ++i) {
        std::uint32_t cp = w[i];
        if (cp >= 0xD800u && cp <= 0xDBFFu && i + 1 < w.size() && w[i + 1] >= 0xDC00u &&
            w[i + 1] <= 0xDFFFu) {
            cp = 0x10000u + ((cp - 0xD800u) << 10) + (w[i + 1] - 0xDC00u);
            ++i;
        } else if (cp >= 0xD800u && cp <= 0xDFFFu) {
            ++bad_count;
            cp = replacement;
        }
        if (cp < 0x80u) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800u) {
            out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        } else if (cp < 0x10000u) {
            out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        } else {
            out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        }
    }
    if (bad) *bad = bad_count;
    return out;
}

}  // namespace lift
