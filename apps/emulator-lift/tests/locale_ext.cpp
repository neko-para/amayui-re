// emulator-lift · 码页与字符串族的 locale 部分（pure：guest 侧自造回调，不碰游戏 / LFS）
//
// 判据（plan §5 A5 / §4 D6 "码页与字符串族"）：
//   ① `GetLocaleInfoW` 按**表**回答：常用 LCType 的字符串 / 数值形态、`LOCALE_RETURN_NUMBER`、
//      `cchData==0` 问大小、缓冲太小 ⇒ 0 + ERROR_INSUFFICIENT_BUFFER、不支持的类型 / LCID ⇒ 0 + 87；
//   ①' `GetLocaleInfoA` 与 W **同表同口径**（差别只在字节数 / CP932 编码 / 数值返回 4）：
//      A 的字节串 == W 的 UTF-16 结果的 CP932 投影；`LOCALE_USE_CP_ACP` 被接受；
//   ② `EnumSystemLocalesA` 的**回调语义**：guest 回调真的被调用（不是"登记了就算"）、
//      参数是 8 位十六进制 LCID 串、顺序确定、回调返回 FALSE 时**停止**后续回调。
//      ★ 这条同时是**框架能力**的判据：shim 里把控制权交回 guest（`ShimCall::take_control` +
//      续延桩）这条路只有这里跑得到 —— 它坏了，这里一定红。
//   ③ `GetStringTypeW`（CT_CTYPE1 的位值 + dwInfoType 不被丢弃）与 `LCMapStringW`
//      （cchSrc=-1 映射整串 / cchDest=0 问大小 / 未实现的 dwMapFlags ⇒ ERROR_INVALID_FLAGS）：
//      REQ-01M6K69JNGE7EP8ZNRDAXNSYZZ / REQ-01M6K69K28AT5VNKZAJXT6PZJ4。
//   ③' 非 ASCII 的 CT_CTYPE1 走**文档化区间规则**（假名 / 全角 / CJK；ASCII 仍走 `ctype1Ascii()`）；
//      CT_CTYPE2/3 仍响亮失败（0 + 87）且**不写**输出缓冲：REQ-01M6KH7EPPNWJDHPDQ6REV2RRM。
//   ③'' LCMapStringW 的 HIRAGANA / KATAKANA / HALFWIDTH / FULLWIDTH / BYTEREV；
//      SORTKEY 仍明确不做（0 + 1008）：REQ-01M6KH7HC3ZP3M5RS8EJ3813WC。
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "fixture.h"
#include "runtime/cp932.h"
#include "runtime/machine.h"
#include "shims/register.h"

namespace {

using lift_fixture::Asm;
using lift_fixture::Fixture;

constexpr std::uint32_t kErrInvalidParameter = 87u;
constexpr std::uint32_t kErrInsufficientBuffer = 122u;
constexpr std::uint32_t kErrInvalidFlags = 1008u;

// GetStringTypeW 的 dwInfoType + LCMapStringW 的 dwMapFlags（Win32 数值）
constexpr std::uint32_t kCtCtype1 = 1u;      // CT_CTYPE1
constexpr std::uint32_t kCtCtype2 = 2u;      // CT_CTYPE2（双向类）
constexpr std::uint32_t kCtCtype3 = 4u;      // CT_CTYPE3（语言学类别）
constexpr std::uint32_t kLcmapLowercase = 0x00000100u;
constexpr std::uint32_t kLcmapUppercase = 0x00000200u;
constexpr std::uint32_t kLcmapSortkey = 0x00000400u;
constexpr std::uint32_t kLcmapByterev = 0x00000800u;
constexpr std::uint32_t kLcmapHiragana = 0x00100000u;
constexpr std::uint32_t kLcmapKatakana = 0x00200000u;
constexpr std::uint32_t kLcmapHalfwidth = 0x00400000u;
constexpr std::uint32_t kLcmapFullwidth = 0x00800000u;

// LCType（与 kernel32_string.cpp 的常量同一组 Win32 数值）
constexpr std::uint32_t kLocaleSISO639LangName = 0x00000059u;
constexpr std::uint32_t kLocaleSName = 0x0000005cu;
constexpr std::uint32_t kLocaleSNativeLangName = 0x00000004u;
constexpr std::uint32_t kLocaleSShortDate = 0x0000001fu;
constexpr std::uint32_t kLocaleIDefaultAnsiCodePage = 0x00001004u;
constexpr std::uint32_t kLocaleReturnNumber = 0x20000000u;
constexpr std::uint32_t kLocaleUseCpAcp = 0x40000000u;
constexpr std::uint32_t kLocaleUserDefault = 0x00000400u;
constexpr std::uint32_t kLocaleJaJp = 0x00000411u;

std::u16string guestWstr(lift::Machine& m, std::uint32_t ea, std::uint32_t cap = 256) {
    std::u16string s;
    for (std::uint32_t i = 0; i < cap; ++i) {
        const char16_t c = static_cast<char16_t>(m.mem().rd16(ea + i * 2u));
        if (c == 0) break;
        s.push_back(c);
    }
    return s;
}

// A 版结果：以结尾 0 为止的字节串
std::string guestAstr(lift::Machine& m, std::uint32_t ea, std::uint32_t cap = 256) {
    std::string s;
    for (std::uint32_t i = 0; i < cap; ++i) {
        const char c = static_cast<char>(m.mem().rd8(ea + i));
        if (c == 0) break;
        s.push_back(c);
    }
    return s;
}

}  // namespace

int main() {
    using namespace lift_test;

    // ① GetLocaleInfoW：字符串 / 数值 / 边界
    {
        g_current_case = "get-locale-info-w";
        Fixture f;
        lift::registerStringShims(f.m.shims());
        LIFT_CHECK(f.ok);
        const std::uint32_t gli = f.use("GetLocaleInfoW");
        const std::uint32_t buf = f.zeroData(256);

        // en-US：ISO 639 语言名 "en" ⇒ 返回 3（含结尾 0）
        LIFT_CHECK_EQ_U32(f.call(gli, {kLocaleUserDefault, kLocaleSISO639LangName, buf, 256u}), 3u);
        LIFT_CHECK(guestWstr(f.m, buf) == u"en");
        // LOCALE_SNAME = "en-US" ⇒ 6
        LIFT_CHECK_EQ_U32(f.call(gli, {kLocaleUserDefault, kLocaleSName, buf, 256u}), 6u);
        LIFT_CHECK(guestWstr(f.m, buf) == u"en-US");
        // ja-JP
        LIFT_CHECK_EQ_U32(f.call(gli, {kLocaleJaJp, kLocaleSISO639LangName, buf, 256u}), 3u);
        LIFT_CHECK(guestWstr(f.m, buf) == u"ja");
        LIFT_CHECK_EQ_U32(f.call(gli, {kLocaleJaJp, kLocaleSName, buf, 256u}), 6u);
        LIFT_CHECK(guestWstr(f.m, buf) == u"ja-JP");

        // cchData==0 ⇒ 所需大小（含结尾 0）
        LIFT_CHECK_EQ_U32(f.call(gli, {kLocaleUserDefault, kLocaleSName, 0u, 0u}), 6u);
        // 缓冲太小 ⇒ 0 + ERROR_INSUFFICIENT_BUFFER（⛔ 不截断）
        LIFT_CHECK_EQ_U32(f.call(gli, {kLocaleUserDefault, kLocaleSName, buf, 3u}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrInsufficientBuffer);

        // 数值形态：LOCALE_IDEFAULTANSICODEPAGE | LOCALE_RETURN_NUMBER ⇒ 返回 2（WCHAR 数）
        const std::uint32_t numbuf = f.u32data(0xDEADBEEFu);
        LIFT_CHECK_EQ_U32(f.call(gli, {kLocaleJaJp,
                                       kLocaleIDefaultAnsiCodePage | kLocaleReturnNumber, numbuf, 2u}),
                          2u);
        LIFT_CHECK_MEM_U32(f.m.mem(), numbuf, 932u);
        LIFT_CHECK_EQ_U32(f.call(gli, {kLocaleUserDefault,
                                       kLocaleIDefaultAnsiCodePage | kLocaleReturnNumber, numbuf, 2u}),
                          2u);
        LIFT_CHECK_MEM_U32(f.m.mem(), numbuf, 1252u);
        // 数值形态 + 缓冲不足 ⇒ 122
        LIFT_CHECK_EQ_U32(f.call(gli, {kLocaleJaJp,
                                       kLocaleIDefaultAnsiCodePage | kLocaleReturnNumber, numbuf, 1u}),
                          0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrInsufficientBuffer);

        // 不支持的类型 / LCID ⇒ 0 + ERROR_INVALID_PARAMETER（⛔ 不返回空串）
        LIFT_CHECK_EQ_U32(f.call(gli, {kLocaleUserDefault, 0x00000099u, buf, 256u}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrInvalidParameter);
        LIFT_CHECK_EQ_U32(f.call(gli, {0x00000407u, kLocaleSName, buf, 256u}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrInvalidParameter);
        // 字符串类型配 LOCALE_RETURN_NUMBER ⇒ 87
        LIFT_CHECK_EQ_U32(f.call(gli, {kLocaleUserDefault, kLocaleSName | kLocaleReturnNumber, numbuf,
                                       4u}),
                          0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrInvalidParameter);
        LIFT_CHECK(f.ok);
    }

    // ①' GetLocaleInfoA：与 W 同表同口径（字节数 / CP932 / 数值返回 4）
    {
        g_current_case = "get-locale-info-a";
        Fixture f;
        lift::registerStringShims(f.m.shims());
        LIFT_CHECK(f.ok);
        const std::uint32_t gliw = f.use("GetLocaleInfoW");
        const std::uint32_t glia = f.use("GetLocaleInfoA");
        const std::uint32_t wbuf = f.zeroData(256);
        const std::uint32_t abuf = f.zeroData(256);

        struct Pair {
            std::uint32_t lcid;
            std::uint32_t type;
        };
        const Pair pairs[] = {
            {kLocaleUserDefault, kLocaleSName},
            {kLocaleJaJp, kLocaleSName},
            {kLocaleJaJp, kLocaleSNativeLangName},  // 日本語（CP932 多字节）
            {kLocaleJaJp, kLocaleSShortDate},       // yy/MM/dd
            {kLocaleUserDefault, kLocaleSISO639LangName},
        };
        for (const Pair& p : pairs) {
            LIFT_CHECK(f.call(gliw, {p.lcid, p.type, wbuf, 256u}) != 0u);
            const std::string expect = lift::wideToCp932(guestWstr(f.m, wbuf));
            // A 的字节串 == W 的 UTF-16 结果的 CP932 投影；返回值 = 字节数（含结尾 0）
            LIFT_CHECK_EQ_U32(f.call(glia, {p.lcid, p.type, abuf, 256u}),
                              static_cast<std::uint32_t>(expect.size() + 1));
            LIFT_CHECK(guestAstr(f.m, abuf) == expect);
            // cchData==0 ⇒ 所需字节数（含结尾 0）—— 与 W 的"字符数"是**不同**的量
            LIFT_CHECK_EQ_U32(f.call(glia, {p.lcid, p.type, 0u, 0u}),
                              static_cast<std::uint32_t>(expect.size() + 1));
        }

        // `LOCALE_USE_CP_ACP` 是 A 版专属 flag：接受、结果不变（本机系统 ACP 与 ja-JP 的 locale ACP 都是 932）
        LIFT_CHECK_EQ_U32(
            f.call(glia, {kLocaleJaJp, kLocaleSNativeLangName | kLocaleUseCpAcp, abuf, 256u}),
            static_cast<std::uint32_t>(lift::wideToCp932(u"日本語").size() + 1));
        LIFT_CHECK(guestAstr(f.m, abuf) == lift::wideToCp932(u"日本語"));

        // 缓冲太小 ⇒ 0 + ERROR_INSUFFICIENT_BUFFER（⛔ 不截断）
        LIFT_CHECK_EQ_U32(f.call(glia, {kLocaleUserDefault, kLocaleSName, abuf, 3u}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrInsufficientBuffer);

        // 数值形态：返回 4（A 版 = sizeof(DWORD)）；cchData==0 ⇒ 4；cap<4 ⇒ 0 + 122
        const std::uint32_t numbuf = f.u32data(0u);
        LIFT_CHECK_EQ_U32(f.call(glia, {kLocaleJaJp,
                                        kLocaleIDefaultAnsiCodePage | kLocaleReturnNumber, numbuf, 4u}),
                          4u);
        LIFT_CHECK_MEM_U32(f.m.mem(), numbuf, 932u);
        LIFT_CHECK_EQ_U32(f.call(glia, {kLocaleJaJp,
                                        kLocaleIDefaultAnsiCodePage | kLocaleReturnNumber, 0u, 0u}),
                          4u);
        LIFT_CHECK_EQ_U32(f.call(glia, {kLocaleJaJp,
                                        kLocaleIDefaultAnsiCodePage | kLocaleReturnNumber, numbuf, 3u}),
                          0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrInsufficientBuffer);

        // 非法 LCID / LCType / flag / NULL 缓冲 ⇒ 与 W **同一套**错误码
        LIFT_CHECK_EQ_U32(f.call(glia, {0x00000407u, kLocaleSName, abuf, 256u}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrInvalidParameter);
        LIFT_CHECK_EQ_U32(f.call(glia, {kLocaleUserDefault, 0x00000099u, abuf, 256u}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrInvalidParameter);
        LIFT_CHECK_EQ_U32(f.call(glia, {kLocaleUserDefault, kLocaleSName | 0x01000000u, abuf, 256u}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrInvalidFlags);
        LIFT_CHECK_EQ_U32(f.call(glia, {kLocaleUserDefault, kLocaleSName, 0u, 8u}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrInvalidParameter);
        LIFT_CHECK(f.ok);
    }

    // ② EnumSystemLocalesA：回调真的被调用 + 参数正确 + FALSE 停止
    {
        g_current_case = "enum-system-locales-callback";
        Fixture f;
        lift::registerStringShims(f.m.shims());
        LIFT_CHECK(f.ok);
        const std::uint32_t enum_loc = f.use("EnumSystemLocalesA");

        // guest 侧记录区
        const std::uint32_t count_ea = f.zeroData(4);
        const std::uint32_t record_ea = f.zeroData(8);  // 两次回调各记开头 4 个字节

        // 主程序：EnumSystemLocalesA(callback, 0) 然后 ret（哨兵）
        const std::uint32_t main_ea = lift_fixture::kCodeBase;
        const std::uint32_t cb_ea = 0x00408000u;
        {
            Asm a(main_ea);
            a.pushImm(0u);      // dwFlags = 0（等价 LCID_SUPPORTED）
            a.pushImm(cb_ea);   // lpLocaleEnumProc
            a.callRel(enum_loc);
            a.ret();
            LIFT_CHECK(f.m.placeCode(main_ea, a.b.data(), a.b.size(), &f.err));
        }
        // 回调：mov ecx,[count]; mov edx,[esp+4]; mov edx,[edx+4];
        //       mov [record + ecx*4], edx; inc dword [count]; mov eax,1; ret 4
        // ★ 文档/实现口径：`EnumLocalesProcA` 收到的是 **8 位十六进制**（带前导 0 的 LCID 串，
        //   例 "00000409"）⇒ 取第 4..7 个字符（`[edx+4]`）正好是 "0409"。
        {
            Asm a(cb_ea);
            a.u8(0x8B); a.u8(0x0D); a.u32(count_ea);              // mov ecx,[count]
            a.u8(0x8B); a.u8(0x54); a.u8(0x24); a.u8(0x04);       // mov edx,[esp+4]
            a.u8(0x8B); a.u8(0x52); a.u8(0x04);                   // mov edx,[edx+4]
            a.u8(0x89); a.u8(0x14); a.u8(0x8D); a.u32(record_ea);  // mov [record+ecx*4],edx
            a.u8(0xFF); a.u8(0x05); a.u32(count_ea);              // inc dword [count]
            a.movEaxImm(1u);                                      // 继续枚举
            a.retImm16(4u);                                       // stdcall 弹 1 个参数
            LIFT_CHECK(f.m.placeCode(cb_ea, a.b.data(), a.b.size(), &f.err));
        }

        f.rearmStack();
        const lift::RunResult r = f.runCode(main_ea);
        LIFT_CHECK(r.reason == lift::StopReason::UntilEa);
        LIFT_CHECK_EQ_U32(f.m.cpu().eax(), 1u);  // 枚举完成 ⇒ TRUE
        LIFT_CHECK_MEM_U32(f.m.mem(), count_ea, 2u);  // 表里两个 locale ⇒ 回调两次
        // 参数是 8 位十六进制 LCID 串，顺序 = 表序（00000409 → 00000411）
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(record_ea), 0x39303430u);      // "0409"
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(record_ea + 4u), 0x31313430u);  // "0411"
        LIFT_CHECK(f.ok);
    }

    // ③ 回调返回 FALSE ⇒ 停止枚举（只回调一次）
    {
        g_current_case = "enum-locales-stop-on-false";
        Fixture f;
        lift::registerStringShims(f.m.shims());
        LIFT_CHECK(f.ok);
        const std::uint32_t enum_loc = f.use("EnumSystemLocalesA");
        const std::uint32_t count_ea = f.zeroData(4);
        const std::uint32_t main_ea = lift_fixture::kCodeBase;
        const std::uint32_t cb_ea = 0x00408000u;
        {
            Asm a(main_ea);
            a.pushImm(2u);  // LCID_SUPPORTED
            a.pushImm(cb_ea);
            a.callRel(enum_loc);
            a.ret();
            LIFT_CHECK(f.m.placeCode(main_ea, a.b.data(), a.b.size(), &f.err));
        }
        {
            Asm a(cb_ea);
            a.u8(0xFF); a.u8(0x05); a.u32(count_ea);  // inc dword [count]
            a.movEaxImm(0u);                          // FALSE ⇒ 停止
            a.retImm16(4u);
            LIFT_CHECK(f.m.placeCode(cb_ea, a.b.data(), a.b.size(), &f.err));
        }
        f.rearmStack();
        const lift::RunResult r = f.runCode(main_ea);
        LIFT_CHECK(r.reason == lift::StopReason::UntilEa);
        LIFT_CHECK_MEM_U32(f.m.mem(), count_ea, 1u);  // 只有第一次
        LIFT_CHECK(f.ok);
    }

    // ④ 非法参数：回调为 NULL / flags 非法
    {
        g_current_case = "enum-locales-bad-args";
        Fixture f;
        lift::registerStringShims(f.m.shims());
        LIFT_CHECK(f.ok);
        const std::uint32_t enum_loc = f.use("EnumSystemLocalesA");
        LIFT_CHECK_EQ_U32(f.call(enum_loc, {0u, 0u}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrInvalidParameter);
        const std::uint32_t cb = 0x00408000u;
        LIFT_CHECK_EQ_U32(f.call(enum_loc, {cb, 8u}), 0u);  // 未知 flag
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, 1008u);
        LIFT_CHECK_EQ_U32(f.call(enum_loc, {cb, 3u}), 0u);  // INSTALLED|SUPPORTED 互斥
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, 1008u);
        LIFT_CHECK(f.ok);
    }

    // ⑤ GetStringTypeW：CT_CTYPE1 的**位值**（REQ-01M6K69JNGE7EP8ZNRDAXNSYZZ）
    //
    // 判据是 MSDN 的 C1_* 表 + 它给的定点：'A'=0x0181、'a'=0x0182、'G'=0x0101、'0'=0x0084、
    // ' '=0x0048、'!'=0x0010。旧实现：'A' 被标成 DIGIT|ALPHA(0x0104)、空格被标成 CNTRL|SPACE(0x0028)、
    // NUL 被标成 C1_UPPER(0x0001) ⇒ 这条会红。
    {
        g_current_case = "get-string-type-w-ctype1";
        Fixture f;
        lift::registerStringShims(f.m.shims());
        lift::registerBootstrapShims(f.m.shims());
        LIFT_CHECK(f.ok);
        const std::uint32_t gst = f.use("GetStringTypeW");
        const std::uint32_t dst = f.zeroData(64);

        // 显式长度路径（cchSrc = 字符数，不含结尾 0）
        const std::u16string s = u"AaG09! ";
        const std::uint32_t src = f.data(s.data(), s.size() * 2u);
        LIFT_CHECK_EQ_U32(f.call(gst, {kCtCtype1, src, static_cast<std::uint32_t>(s.size()), dst}), 1u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd16(dst + 0u), 0x0181u);  // 'A' = UPPER|ALPHA|XDIGIT
        LIFT_CHECK_EQ_U32(f.m.mem().rd16(dst + 2u), 0x0182u);  // 'a' = LOWER|ALPHA|XDIGIT
        LIFT_CHECK_EQ_U32(f.m.mem().rd16(dst + 4u), 0x0101u);  // 'G' = UPPER|ALPHA（G 之后不是 XDIGIT）
        LIFT_CHECK_EQ_U32(f.m.mem().rd16(dst + 6u), 0x0084u);  // '0' = DIGIT|XDIGIT
        LIFT_CHECK_EQ_U32(f.m.mem().rd16(dst + 8u), 0x0084u);  // '9'
        LIFT_CHECK_EQ_U32(f.m.mem().rd16(dst + 10u), 0x0010u); // '!' = PUNCT
        LIFT_CHECK_EQ_U32(f.m.mem().rd16(dst + 12u), 0x0048u); // ' ' = SPACE|BLANK

        // 控制字符：NUL / CR 都是 C1_CNTRL（旧实现给 NUL 0x0001 = C1_UPPER！）
        // ★ 不能用 u"\0\r"：`std::u16string` 从字面量构造会在第一个 0 处停下 ⇒ 逐字符 push
        std::u16string ctl;
        ctl.push_back(static_cast<char16_t>(0x0000));
        ctl.push_back(static_cast<char16_t>(0x000D));
        const std::uint32_t ctl_ea = f.data(ctl.data(), ctl.size() * 2u);
        LIFT_CHECK_EQ_U32(f.call(gst, {kCtCtype1, ctl_ea, 2u, dst}), 1u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd16(dst + 0u), 0x0020u);  // U+0000 = CNTRL
        LIFT_CHECK_EQ_U32(f.m.mem().rd16(dst + 2u), 0x0020u);  // U+000D = CNTRL

        // cchSrc == -1：NUL 结尾（结尾的 0 不参与分类）
        const std::u16string two = u"Az";
        const std::uint32_t two_ea = f.data(two.data(), (two.size() + 1) * 2u);
        LIFT_CHECK_EQ_U32(f.call(gst, {kCtCtype1, two_ea, 0xFFFFFFFFu, dst}), 1u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd16(dst + 0u), 0x0181u);  // 'A' = UPPER|ALPHA|XDIGIT
        LIFT_CHECK_EQ_U32(f.m.mem().rd16(dst + 2u), 0x0102u);  // 'z' = LOWER|ALPHA（z 不是十六进制位）

        // 非 ASCII：走 `kernel32_bootstrap.cpp` 的**文档化区间规则**（`ctype1NonAscii`）。
        // ★ 判据是 MSDN 的 C1_* 表 + 它明写「hiragana / katakana / kanji all have C1_ALPHA」；
        //   ⛔ 不拿属性现算 / 不查外部大表。
        struct Ctype1Case {
            char16_t ch;
            std::uint16_t bits;
        };
        const Ctype1Case non_ascii[] = {
            {static_cast<char16_t>(0x3000), 0x0048},  // 全角空格 = SPACE|BLANK
            {static_cast<char16_t>(0x3001), 0x0010},  // 日文标点 、= PUNCT
            {static_cast<char16_t>(0x303F), 0x0010},  // 区间上界 = PUNCT
            {static_cast<char16_t>(0x3042), 0x0100},  // 平假名 あ = ALPHA
            {static_cast<char16_t>(0x30A2), 0x0100},  // 片假名 ア = ALPHA
            {static_cast<char16_t>(0x4E00), 0x0100},  // 汉字 一 = ALPHA
            {static_cast<char16_t>(0xFF01), 0x0010},  // 全角 ！= PUNCT（映回 '!'）
            {static_cast<char16_t>(0xFF21), 0x0181},  // 全角 Ａ = 与 ASCII 'A' 同一位（含 XDIGIT）
            {static_cast<char16_t>(0xFF41), 0x0182},  // 全角 ａ = 与 ASCII 'a' 同一位
            {static_cast<char16_t>(0xFF10), 0x0084},  // 全角 ０ = DIGIT|XDIGIT
            {static_cast<char16_t>(0xFF5E), 0x0010},  // 全角 ～ = PUNCT（映回 '~'）
            {static_cast<char16_t>(0xFF61), 0x0010},  // 半角 ｡ = PUNCT（Unicode Po）
            {static_cast<char16_t>(0xFF64), 0x0010},  // 半角 ､ = PUNCT（Unicode Po）
            {static_cast<char16_t>(0xFF65), 0x0010},  // 半角 ･ = PUNCT（Unicode Po）
            {static_cast<char16_t>(0xFF66), 0x0100},  // 半角 ｦ = ALPHA
            {static_cast<char16_t>(0xFF71), 0x0100},  // 半角 ｱ = ALPHA
            {static_cast<char16_t>(0xFF9E), 0x0100},  // 半角 ﾞ = ALPHA
        };
        std::u16string non_ascii_text;
        for (const Ctype1Case& c : non_ascii) non_ascii_text.push_back(c.ch);
        const std::uint32_t non_ascii_ea =
            f.data(non_ascii_text.data(), non_ascii_text.size() * 2u);
        LIFT_CHECK_EQ_U32(f.call(gst, {kCtCtype1, non_ascii_ea,
                                       static_cast<std::uint32_t>(non_ascii_text.size()), dst}),
                          1u);
        for (std::size_t i = 0; i < non_ascii_text.size(); ++i) {
            LIFT_CHECK_EQ_U32(f.m.mem().rd16(dst + static_cast<std::uint32_t>(i) * 2u),
                              non_ascii[i].bits);
        }
        LIFT_CHECK(f.ok);
    }

    // ⑤' dwInfoType 不被丢弃：只支持 CT_CTYPE1，其余 ⇒ 0 + ERROR_INVALID_PARAMETER（⛔ 不许假装成功）
    {
        g_current_case = "get-string-type-w-info-type";
        Fixture f;
        lift::registerStringShims(f.m.shims());
        lift::registerBootstrapShims(f.m.shims());
        LIFT_CHECK(f.ok);
        const std::uint32_t gst = f.use("GetStringTypeW");
        const std::uint32_t dst = f.zeroData(64);
        const std::u16string s = u"A";
        const std::uint32_t src = f.data(s.data(), 2u);

        // CT_CTYPE2(2) / CT_CTYPE3(4) / CT_CTYPE1|CT_CTYPE2(3) / 0：都不是本模拟器做的
        // ⇒ 0 + ERROR_INVALID_PARAMETER，而且**不写** lpCharType（⛔ 不拿 CT_CTYPE1 的位冒充）。
        // ★ 这是"CT_CTYPE2/3 明确不做"的响亮失败钉子，合同见 docs/03-emulator/contracts-win32.md。
        for (std::uint32_t type : {0u, kCtCtype2, kCtCtype3, kCtCtype1 | kCtCtype2}) {
            f.m.cpu().shim.last_error = 0;
            f.m.mem().wr32(dst, 0xDEADBEEFu);
            f.m.mem().wr32(dst + 4u, 0xDEADBEEFu);
            LIFT_CHECK_EQ_U32(f.call(gst, {type, src, 1u, dst}), 0u);
            LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrInvalidParameter);
            LIFT_CHECK_MEM_U32(f.m.mem(), dst, 0xDEADBEEFu);
            LIFT_CHECK_MEM_U32(f.m.mem(), dst + 4u, 0xDEADBEEFu);
        }
        // 参数非法：lpSrcStr = 0 / lpCharType = 0 / cchSrc = 0 / cchSrc = -2
        f.m.cpu().shim.last_error = 0;
        LIFT_CHECK_EQ_U32(f.call(gst, {kCtCtype1, 0u, 1u, dst}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrInvalidParameter);
        LIFT_CHECK_EQ_U32(f.call(gst, {kCtCtype1, src, 1u, 0u}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrInvalidParameter);
        LIFT_CHECK_EQ_U32(f.call(gst, {kCtCtype1, src, 0u, dst}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrInvalidParameter);
        LIFT_CHECK_EQ_U32(f.call(gst, {kCtCtype1, src, 0xFFFFFFFEu, dst}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrInvalidParameter);
        LIFT_CHECK(f.ok);
    }

    // ⑥ LCMapStringW 的 cchSrc == -1（REQ-01M6K69K28AT5VNKZAJXT6PZJ4）
    //
    // 判据：`"AbC"` + LCMAP_LOWERCASE + cchSrc=-1 ⇒ 返回 4（**含**结尾 0）、dst = L"abc\0"；
    // cchDest==0 是"问大小"（同一数值）；缓冲不足 ⇒ 0 + 122；显式 cchSrc=3 不回归。
    // ★ 旧实现 cchSrc<0 只映射 1 个字符并返回 1 ⇒ 这条会红。
    {
        g_current_case = "lcmap-string-null-terminated";
        Fixture f;
        lift::registerStringShims(f.m.shims());
        lift::registerBootstrapShims(f.m.shims());
        LIFT_CHECK(f.ok);
        const std::uint32_t lcm = f.use("LCMapStringW");
        const std::uint32_t dst = f.zeroData(64);

        const std::u16string abc = u"AbC";
        const std::uint32_t src = f.data(abc.data(), (abc.size() + 1) * 2u);
        LIFT_CHECK_EQ_U32(
            f.call(lcm, {kLocaleUserDefault, kLcmapLowercase, src, 0xFFFFFFFFu, dst, 64u}), 4u);
        LIFT_CHECK(guestWstr(f.m, dst) == u"abc");
        LIFT_CHECK_EQ_U32(f.m.mem().rd16(dst + 6u), 0u);  // 结尾 0 也写进去了

        // cchDest == 0：只问大小，返回同一数值（旧实现返回 1）
        LIFT_CHECK_EQ_U32(f.call(lcm, {kLocaleUserDefault, kLcmapLowercase, src, 0xFFFFFFFFu, dst, 0u}), 4u);
        LIFT_CHECK_EQ_U32(f.call(lcm, {kLocaleUserDefault, kLcmapLowercase, src, 0xFFFFFFFFu, 0u, 0u}), 4u);

        // 缓冲不足：需要 4 个字符，只给 3 ⇒ 0 + ERROR_INSUFFICIENT_BUFFER
        f.m.cpu().shim.last_error = 0;
        LIFT_CHECK_EQ_U32(
            f.call(lcm, {kLocaleUserDefault, kLcmapLowercase, src, 0xFFFFFFFFu, dst, 3u}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrInsufficientBuffer);
        LIFT_CHECK(guestWstr(f.m, dst) == u"abc");  // 失败不写坏已有内容

        // 显式 cchSrc = 3（不回归）：3 个字符全映射、返回 3
        LIFT_CHECK_EQ_U32(f.call(lcm, {kLocaleUserDefault, kLcmapLowercase, src, 3u, dst, 64u}), 3u);
        LIFT_CHECK(guestWstr(f.m, dst) == u"abc");

        // UPPERCASE 同口径
        const std::u16string mixed = u"aBc";
        const std::uint32_t up = f.data(mixed.data(), (mixed.size() + 1) * 2u);
        LIFT_CHECK_EQ_U32(
            f.call(lcm, {kLocaleUserDefault, kLcmapUppercase, up, 0xFFFFFFFFu, dst, 64u}), 4u);
        LIFT_CHECK(guestWstr(f.m, dst) == u"ABC");
        LIFT_CHECK(f.ok);
    }

    // ⑥' 未实现的 dwMapFlags ⇒ 0 + ERROR_INVALID_FLAGS（⛔ 旧实现静默按"无映射"成功）
    {
        g_current_case = "lcmap-string-invalid-flags";
        Fixture f;
        lift::registerStringShims(f.m.shims());
        lift::registerBootstrapShims(f.m.shims());
        LIFT_CHECK(f.ok);
        const std::uint32_t lcm = f.use("LCMapStringW");
        const std::uint32_t dst = f.zeroData(64);
        const std::u16string abc = u"AbC";
        const std::uint32_t src = f.data(abc.data(), (abc.size() + 1) * 2u);

        // 仍明确不做的位：LCMAP_SORTKEY(0x400) / LCMAP_LINGUISTIC_CASING(0x1000000)
        for (std::uint32_t flags : {kLcmapSortkey, 0x01000000u}) {
            f.m.cpu().shim.last_error = 0;
            LIFT_CHECK_EQ_U32(f.call(lcm, {kLocaleUserDefault, flags, src, 0xFFFFFFFFu, dst, 64u}), 0u);
            LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrInvalidFlags);
        }
        // 文档点名的互斥对：LOWERCASE|UPPERCASE / HIRAGANA|KATAKANA / HALFWIDTH|FULLWIDTH ⇒ 1008
        for (std::uint32_t flags : {kLcmapLowercase | kLcmapUppercase,
                                    kLcmapHiragana | kLcmapKatakana,
                                    kLcmapHalfwidth | kLcmapFullwidth}) {
            f.m.cpu().shim.last_error = 0;
            LIFT_CHECK_EQ_U32(f.call(lcm, {kLocaleUserDefault, flags, src, 0xFFFFFFFFu, dst, 64u}), 0u);
            LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrInvalidFlags);
        }

        // dwMapFlags = 0 是合法的"不映射"（原样复制 + 同一个返回值）
        LIFT_CHECK_EQ_U32(f.call(lcm, {kLocaleUserDefault, 0u, src, 0xFFFFFFFFu, dst, 64u}), 4u);
        LIFT_CHECK(guestWstr(f.m, dst) == u"AbC");
        LIFT_CHECK(f.ok);
    }

    // ⑥'' LCMapStringW 的假名 / 全半角 / BYTEREV（REQ-01M6KH7HC3ZP3M5RS8EJ3813WC，单 2）
    //
    // 判据：HIRAGANA 把 ア→あ、KATAKANA 把 あ→ア；FULLWIDTH 把 A→Ａ、HALFWIDTH 把 Ａ→A
    // （各一条往返）；半角片假名 U+FF61–U+FF9F 共 63 项全部 FULLWIDTH→HALFWIDTH 往返；
    // BYTEREV 用 MSDN 的定点 0x3450 0x4822 ⇒ 0x5034 0x2248（逐 16 位码元反转字节，不是反转
    // 字符顺序）；文档明说 HIRAGANA 不配 FULLWIDTH 时半角片假名不映射。
    // ★ 映射规则在 `kernel32_bootstrap.cpp`（假名 ±0x60 区间 + 63 项 Unicode 标准 1:1 表）；
    //   这里只钉调用方看得见的结果。
    {
        g_current_case = "lcmap-string-kana-width";
        Fixture f;
        lift::registerStringShims(f.m.shims());
        lift::registerBootstrapShims(f.m.shims());
        LIFT_CHECK(f.ok);
        const std::uint32_t lcm = f.use("LCMapStringW");
        const std::uint32_t dst = f.zeroData(64);

        // ★ 按 **cap 个字符**读回（不是读到 NUL）：显式 cchSrc 的映射不写结尾 0，
        //   复用同一个 dst 时旧尾巴会留在那里。
        auto map = [&](const std::u16string& in, std::uint32_t flags) -> std::u16string {
            const std::uint32_t src = f.data(in.data(), in.size() * 2u);
            const std::uint32_t cap = static_cast<std::uint32_t>(in.size());
            LIFT_CHECK_EQ_U32(f.call(lcm, {kLocaleUserDefault, flags, src, cap, dst, 64u}), cap);
            std::u16string out;
            for (std::uint32_t i = 0; i < cap; ++i)
                out.push_back(static_cast<char16_t>(f.m.mem().rd16(dst + i * 2u)));
            return out;
        };

        // 假名：各一条往返（あ⇄ア）
        LIFT_CHECK(map(u"\u3042", kLcmapKatakana) == u"\u30A2");
        LIFT_CHECK(map(u"\u30A2", kLcmapHiragana) == u"\u3042");
        LIFT_CHECK(map(map(u"\u3042", kLcmapKatakana), kLcmapHiragana) == u"\u3042");
        // 偏移区间内的特殊假名（ゔ / ゕ / ゖ）也按 ±0x60 走
        LIFT_CHECK(map(u"\u3094", kLcmapKatakana) == u"\u30F4");  // ゔ ⇄ ヴ
        LIFT_CHECK(map(u"\u30F4", kLcmapHiragana) == u"\u3094");
        LIFT_CHECK(map(u"\u3095", kLcmapKatakana) == u"\u30F5");  // ゕ ⇄ ヵ
        LIFT_CHECK(map(u"\u30F5", kLcmapHiragana) == u"\u3095");
        LIFT_CHECK(map(u"\u3096", kLcmapKatakana) == u"\u30F6");  // ゖ ⇄ ヶ
        LIFT_CHECK(map(u"\u30F6", kLcmapHiragana) == u"\u3096");
        // 共享/特殊形态：ー 与组合浊音点不动；ゝゞゟ ⇄ ヽヾヿ（同一 ±0x60）
        LIFT_CHECK(map(u"\u30FC", kLcmapHiragana) == u"\u30FC");  // ー 是平/片假名共享
        LIFT_CHECK(map(u"\u3099", kLcmapKatakana) == u"\u3099");  // 组合浊音点共享
        LIFT_CHECK(map(u"\u309D", kLcmapKatakana) == u"\u30FD");  // ゝ → ヽ
        LIFT_CHECK(map(u"\u30FD", kLcmapHiragana) == u"\u309D");
        // 全半角：各一条往返（A⇄Ａ）
        LIFT_CHECK(map(u"A", kLcmapFullwidth) == u"\uFF21");
        LIFT_CHECK(map(u"\uFF21", kLcmapHalfwidth) == u"A");
        LIFT_CHECK(map(map(u"A", kLcmapFullwidth), kLcmapHalfwidth) == u"A");
        // 同一张宽度表也管半角片假名：ｱ⇄ア
        LIFT_CHECK(map(u"\uFF71", kLcmapFullwidth) == u"\u30A2");
        LIFT_CHECK(map(u"\u30A2", kLcmapHalfwidth) == u"\uFF71");
        // ★ 63 项全部往返：U+FF61–U+FF9F 逐项 FULLWIDTH → HALFWIDTH 回到原字符，且全角 ≠ 半角
        std::uint32_t width_cases = 0;
        for (std::uint32_t cp = 0xFF61; cp <= 0xFF9F; ++cp) {
            const std::u16string half(1, static_cast<char16_t>(cp));
            const std::u16string full = map(half, kLcmapFullwidth);
            LIFT_CHECK_EQ_U32(full.size(), 1u);
            LIFT_CHECK(full[0] != static_cast<char16_t>(cp));
            LIFT_CHECK(map(full, kLcmapHalfwidth) == half);
            ++width_cases;
        }
        LIFT_CHECK_EQ_U32(width_cases, 63u);
        // 文档：HIRAGANA 不配 FULLWIDTH 时，半角片假名原样留在目标串里；配 FULLWIDTH 才映到平假名
        LIFT_CHECK(map(u"\uFF71", kLcmapHiragana) == u"\uFF71");
        LIFT_CHECK(map(u"\uFF71", kLcmapHiragana | kLcmapFullwidth) == u"\u3042");
        // BYTEREV：MSDN 的定点（逐 16 位码元反转字节）
        LIFT_CHECK(map(u"\u3450\u4822", kLcmapByterev) == u"\u5034\u2248");
        // BYTEREV 与映射可组合（先映射再反转字节）：'A' ⇒ 'a' ⇒ 0x6100
        LIFT_CHECK(map(u"A", kLcmapLowercase | kLcmapByterev) == u"\u6100");
        LIFT_CHECK(f.ok);
    }

    // ⑦ ★ 码页叶子的**方向对称**（2026-11-05 缺陷单 `🐞S2 日文 guest 路径乱码` 的单元级守卫）：
    //    `utf8ToCp932` 是"A 版 API 出口"的唯一编码实现，`cp932ToWide` 是入口的解码实现。
    //    判据：`天結` 的 CP932 就是 **93 56 8C 8B**（硬编码，⛔ 不用自己现算）、UTF-8 是 6 字节，
    //    两条互逆；截断只在**字符边界**上发生。
    {
        g_current_case = "cp932-byte-direction";
        const std::string utf8 = "天結";  // UTF-8: E5 A4 A9 E7 B5 90
        const char kCp932[4] = {'\x93', '\x56', '\x8C', '\x8B'};
        const std::string want(kCp932, sizeof(kCp932));
        LIFT_CHECK(utf8.size() == 6u);
        std::size_t bad = 0;
        const std::string bytes = lift::utf8ToCp932(utf8, &bad);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(bad), 0u);
        LIFT_CHECK(bytes == want);
        // 反向：CP932 字节 → UTF-16（= A 版入参的解码方向）
        LIFT_CHECK(lift::cp932ToWide(bytes) == u"天結");
        // 截断：3 字节的预算只放得下 1 个双字节字符（⛔ 不留半个）
        LIFT_CHECK(lift::cp932Truncate(bytes, 3) == want.substr(0, 2));
        LIFT_CHECK(lift::cp932Truncate(bytes, 4) == want);
        // ★ 边界正好落在**第二个字符的 trail** 上：`天` + `あ`（CP932 93 56 / 82 A0）截到 4 字节
        //   必须两个字符都留下 —— 只看"末字节像不像首字节"的旧写法会多丢一个（trail 0xA0 也在
        //   0x81..0xFC 里）。
        const char kPa[4] = {'\x93', '\x56', '\x82', '\xA0'};
        const std::string tenpa(kPa, sizeof(kPa));
        LIFT_CHECK(lift::cp932ToWide(tenpa) == u"天あ");
        LIFT_CHECK(lift::cp932Truncate(tenpa, 4) == tenpa);
        LIFT_CHECK(lift::cp932Truncate(tenpa, 3) == tenpa.substr(0, 2));
    }

    return finish("locale-ext");
}
