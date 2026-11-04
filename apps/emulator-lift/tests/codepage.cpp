// emulator-lift · 码页族：MultiByteToWideChar / WideCharToMultiByte（pure：guest 侧自造，不碰游戏）
//
// 判据（plan §4 D6 "码页与字符串族"）：
//   ① 932 走真表：0x82A0 → U+3042、反向 2 字节、半角片假名；⛔ 不再是"字节值直通"。
//   ② 长度语义：cbMultiByte 是**字节**、cchWideChar 是**字符**；目标为空 / 0 是"问大小"，
//      `-1` 含结尾 0；缓冲不足 ⇒ 0 + ERROR_INSUFFICIENT_BUFFER。
//   ③ 非法序列：默认 U+FFFD（只前进 1 字节）；带 MB_ERR_INVALID_CHARS ⇒ 0 + 1113。
//   ④ W→A：不可编码 ⇒ lpDefaultChar（默认 '?'）+ *lpUsedDefaultChar=1；可编码时写回 0；
//      代理对 / 孤立代理按替换字符；带 WC_ERR_INVALID_CHARS ⇒ 0 + 1113。
//   ⑤ CP_UTF8：UTF-8 ↔ UTF-16 往返。
// ★ 走真实 shim 派发（不是直接调 C++ 函数），与 strfmt / locale-ext 同形。
#include <cstdint>
#include <string>

#include "check.h"
#include "fixture.h"
#include "runtime/machine.h"
#include "shims/register.h"

namespace {

using lift_fixture::Fixture;

constexpr std::uint32_t kCp932 = 932;
constexpr std::uint32_t kCpUtf8 = 65001;
constexpr std::uint32_t kErrInvalidParameter = 87;
constexpr std::uint32_t kErrInsufficientBuffer = 122;
constexpr std::uint32_t kErrNoUnicodeTranslation = 1113;
constexpr std::uint32_t kMbErrInvalidChars = 0x00000008;
constexpr std::uint32_t kWcErrInvalidChars = 0x00000080;

std::string guestBytes(lift::Machine& m, std::uint32_t ea, std::size_t n) {
    std::string s;
    for (std::size_t i = 0; i < n; ++i) s.push_back(static_cast<char>(m.mem().rd8(ea + static_cast<std::uint32_t>(i))));
    return s;
}

// 放一个以 0 结尾的 UTF-16 串（含结尾 0）
std::uint32_t putWide(Fixture& f, const std::u16string& s) { return f.data(s.data(), (s.size() + 1) * 2); }

}  // namespace

int main() {
    using namespace lift_test;

    // ① 932 字节 → UTF-16
    {
        g_current_case = "mb-to-wide-932";
        Fixture f;
        lift::registerStringShims(f.m.shims());
        lift::registerBootstrapShims(f.m.shims());
        LIFT_CHECK(f.ok);
        const std::uint32_t mb = f.use("MultiByteToWideChar");
        const std::uint32_t buf = f.zeroData(64);
        const std::uint32_t src = f.cstr("\x82\xA0");  // あ

        LIFT_CHECK_EQ_U32(f.call(mb, {kCp932, 0u, src, 2u, buf, 32u}), 1u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd16(buf), 0x3042u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd16(buf + 2u), 0u);

        // 目标为空 / 长度为 0 ⇒ 返回**字符**数（不是字节数）
        LIFT_CHECK_EQ_U32(f.call(mb, {kCp932, 0u, src, 2u, 0u, 0u}), 1u);
        LIFT_CHECK_EQ_U32(f.call(mb, {kCp932, 0u, src, 2u, buf, 0u}), 1u);
        // -1 ⇒ 含结尾 0
        LIFT_CHECK_EQ_U32(f.call(mb, {kCp932, 0u, src, 0xFFFFFFFFu, buf, 32u}), 2u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd16(buf + 2u), 0u);
        // 缓冲不足（需要 2 个字符，只给 1）⇒ 0 + 122
        LIFT_CHECK_EQ_U32(f.call(mb, {kCp932, 0u, src, 0xFFFFFFFFu, buf, 1u}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrInsufficientBuffer);
        // ASCII
        const std::uint32_t abc = f.cstr("abc");
        LIFT_CHECK_EQ_U32(f.call(mb, {kCp932, 0u, abc, 0xFFFFFFFFu, buf, 32u}), 4u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd16(buf), 'a');
        LIFT_CHECK_EQ_U32(f.m.mem().rd16(buf + 6u), 0u);
        // cbMultiByte == 0 是失败（不是"转出 0 个字符"）
        LIFT_CHECK_EQ_U32(f.call(mb, {kCp932, 0u, src, 0u, buf, 32u}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrInvalidParameter);
    }

    // ② 非法序列：默认 U+FFFD / 严格模式 1113
    {
        g_current_case = "mb-to-wide-invalid";
        Fixture f;
        lift::registerStringShims(f.m.shims());
        lift::registerBootstrapShims(f.m.shims());
        LIFT_CHECK(f.ok);
        const std::uint32_t mb = f.use("MultiByteToWideChar");
        const std::uint32_t buf = f.zeroData(64);
        const std::uint32_t bad = f.cstr("\x81\x20");  // 0x81 0x20 不是合法 CP932 pair

        LIFT_CHECK_EQ_U32(f.call(mb, {kCp932, 0u, bad, 2u, buf, 32u}), 2u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd16(buf), 0xFFFDu);       // 非法 lead ⇒ 一个替换字符
        LIFT_CHECK_EQ_U32(f.m.mem().rd16(buf + 2u), 0x0020u);  // 后面的空格照常解
        LIFT_CHECK_EQ_U32(f.call(mb, {kCp932, kMbErrInvalidChars, bad, 2u, buf, 32u}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrNoUnicodeTranslation);
        // 末尾孤立 lead 字节
        const std::uint32_t lead = f.cstr("\x82");
        LIFT_CHECK_EQ_U32(f.call(mb, {kCp932, 0u, lead, 1u, buf, 32u}), 1u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd16(buf), 0xFFFDu);
    }

    // ③ CP_UTF8：UTF-8 → UTF-16
    {
        g_current_case = "mb-to-wide-utf8";
        Fixture f;
        lift::registerStringShims(f.m.shims());
        lift::registerBootstrapShims(f.m.shims());
        LIFT_CHECK(f.ok);
        const std::uint32_t mb = f.use("MultiByteToWideChar");
        const std::uint32_t buf = f.zeroData(64);
        const std::uint32_t src = f.cstr("\xE3\x81\x82");  // あ（UTF-8）
        LIFT_CHECK_EQ_U32(f.call(mb, {kCpUtf8, 0u, src, 3u, buf, 32u}), 1u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd16(buf), 0x3042u);
        LIFT_CHECK_EQ_U32(f.call(mb, {kCpUtf8, 0u, src, 0xFFFFFFFFu, buf, 32u}), 2u);
        // 非法 UTF-8：默认 U+FFFD；严格模式 1113
        const std::uint32_t bad = f.cstr("\xFF");
        LIFT_CHECK_EQ_U32(f.call(mb, {kCpUtf8, 0u, bad, 1u, buf, 32u}), 1u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd16(buf), 0xFFFDu);
        LIFT_CHECK_EQ_U32(f.call(mb, {kCpUtf8, kMbErrInvalidChars, bad, 1u, buf, 32u}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrNoUnicodeTranslation);
    }

    // ④ UTF-16 → 932
    {
        g_current_case = "wide-to-mb-932";
        Fixture f;
        lift::registerStringShims(f.m.shims());
        lift::registerBootstrapShims(f.m.shims());
        LIFT_CHECK(f.ok);
        const std::uint32_t wc = f.use("WideCharToMultiByte");
        const std::uint32_t buf = f.zeroData(64);
        const std::uint32_t src = putWide(f, u"\u3042\u30BD");  // あソ

        LIFT_CHECK_EQ_U32(f.call(wc, {kCp932, 0u, src, 2u, buf, 32u, 0u, 0u}), 4u);
        LIFT_CHECK(guestBytes(f.m, buf, 4) == std::string("\x82\xA0\x83\x5C"));
        // 问大小返回**字节**数
        LIFT_CHECK_EQ_U32(f.call(wc, {kCp932, 0u, src, 2u, 0u, 0u, 0u, 0u}), 4u);
        LIFT_CHECK_EQ_U32(f.call(wc, {kCp932, 0u, src, 2u, buf, 0u, 0u, 0u}), 4u);
        // -1 ⇒ 含结尾 0
        LIFT_CHECK_EQ_U32(f.call(wc, {kCp932, 0u, src, 0xFFFFFFFFu, buf, 32u, 0u, 0u}), 5u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd8(buf + 4u), 0u);
        // 缓冲不足（需要 5 字节，只给 4）⇒ 0 + 122
        LIFT_CHECK_EQ_U32(f.call(wc, {kCp932, 0u, src, 0xFFFFFFFFu, buf, 4u, 0u, 0u}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrInsufficientBuffer);
        // 半角片假名
        const std::uint32_t half = putWide(f, u"\uFF71");  // ｱ
        LIFT_CHECK_EQ_U32(f.call(wc, {kCp932, 0u, half, 1u, buf, 8u, 0u, 0u}), 1u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd8(buf), 0xB1u);
        // cchWideChar == 0 是失败
        LIFT_CHECK_EQ_U32(f.call(wc, {kCp932, 0u, src, 0u, buf, 32u, 0u, 0u}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrInvalidParameter);
    }

    // ⑤ 不可编码 ⇒ lpDefaultChar / lpUsedDefaultChar
    {
        g_current_case = "wide-to-mb-default-char";
        Fixture f;
        lift::registerStringShims(f.m.shims());
        lift::registerBootstrapShims(f.m.shims());
        LIFT_CHECK(f.ok);
        const std::uint32_t wc = f.use("WideCharToMultiByte");
        const std::uint32_t buf = f.zeroData(64);
        const std::uint32_t used = f.u32data(0xDEADBEEFu);
        const std::uint32_t snow = putWide(f, u"\u2603");  // 雪（CP932 里没有）

        LIFT_CHECK_EQ_U32(f.call(wc, {kCp932, 0u, snow, 1u, buf, 8u, 0u, used}), 1u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd8(buf), '?');
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(used), 1u);
        // 自定义默认字符
        const std::uint32_t hash = f.cstr("#");
        LIFT_CHECK_EQ_U32(f.call(wc, {kCp932, 0u, snow, 1u, buf, 8u, hash, used}), 1u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd8(buf), '#');
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(used), 1u);
        // 可编码时不置 used
        const std::uint32_t ok = putWide(f, u"\u3042");
        LIFT_CHECK_EQ_U32(f.call(wc, {kCp932, 0u, ok, 1u, buf, 8u, 0u, used}), 2u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(used), 0u);
        // 增补面（代理对）在 CP932 里没有 ⇒ 一个 '?'（不是两个）
        const std::uint32_t pair = putWide(f, u"\U0001F600");
        LIFT_CHECK_EQ_U32(f.call(wc, {kCp932, 0u, pair, 2u, buf, 8u, 0u, used}), 1u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd8(buf), '?');
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(used), 1u);
        // 孤立高代理 + 严格模式 ⇒ 0 + 1113
        std::u16string lone;
        lone.push_back(static_cast<char16_t>(0xD800));
        const std::uint32_t lone_ea = f.data(lone.data(), 2u);
        LIFT_CHECK_EQ_U32(f.call(wc, {kCp932, kWcErrInvalidChars, lone_ea, 1u, buf, 8u, 0u, used}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrNoUnicodeTranslation);
    }

    // ⑥ CP_UTF8：UTF-16 → UTF-8
    {
        g_current_case = "wide-to-mb-utf8";
        Fixture f;
        lift::registerStringShims(f.m.shims());
        lift::registerBootstrapShims(f.m.shims());
        LIFT_CHECK(f.ok);
        const std::uint32_t wc = f.use("WideCharToMultiByte");
        const std::uint32_t buf = f.zeroData(64);
        const std::uint32_t src = putWide(f, u"\u3042");
        LIFT_CHECK_EQ_U32(f.call(wc, {kCpUtf8, 0u, src, 1u, buf, 32u, 0u, 0u}), 3u);
        LIFT_CHECK(guestBytes(f.m, buf, 3) == std::string("\xE3\x81\x82"));
        LIFT_CHECK_EQ_U32(f.call(wc, {kCpUtf8, 0u, src, 0xFFFFFFFFu, buf, 32u, 0u, 0u}), 4u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd8(buf + 3u), 0u);
        // 孤立代理：默认写 U+FFFD（EF BF BD）；严格模式 1113
        std::u16string lone;
        lone.push_back(static_cast<char16_t>(0xDC00));
        const std::uint32_t lone_ea = f.data(lone.data(), 2u);
        LIFT_CHECK_EQ_U32(f.call(wc, {kCpUtf8, 0u, lone_ea, 1u, buf, 32u, 0u, 0u}), 3u);
        LIFT_CHECK(guestBytes(f.m, buf, 3) == std::string("\xEF\xBF\xBD"));
        LIFT_CHECK_EQ_U32(f.call(wc, {kCpUtf8, kWcErrInvalidChars, lone_ea, 1u, buf, 32u, 0u, 0u}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrNoUnicodeTranslation);
    }

    return finish("codepage");
}
