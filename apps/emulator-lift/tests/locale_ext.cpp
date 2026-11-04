// emulator-lift · 码页与字符串族的 locale 部分（pure：guest 侧自造回调，不碰游戏 / LFS）
//
// 判据（plan §5 A5 / §4 D6 "码页与字符串族"）：
//   ① `GetLocaleInfoW` 按**表**回答：常用 LCType 的字符串 / 数值形态、`LOCALE_RETURN_NUMBER`、
//      `cchData==0` 问大小、缓冲太小 ⇒ 0 + ERROR_INSUFFICIENT_BUFFER、不支持的类型 / LCID ⇒ 0 + 87；
//   ② `EnumSystemLocalesA` 的**回调语义**：guest 回调真的被调用（不是"登记了就算"）、
//      参数是 8 位十六进制 LCID 串、顺序确定、回调返回 FALSE 时**停止**后续回调。
//      ★ 这条同时是**框架能力**的判据：shim 里把控制权交回 guest（`ShimCall::take_control` +
//      续延桩）这条路只有这里跑得到 —— 它坏了，这里一定红。
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "fixture.h"
#include "runtime/machine.h"
#include "shims/register.h"

namespace {

using lift_fixture::Asm;
using lift_fixture::Fixture;

constexpr std::uint32_t kErrInvalidParameter = 87u;
constexpr std::uint32_t kErrInsufficientBuffer = 122u;

// LCType（与 kernel32_string.cpp 的常量同一组 Win32 数值）
constexpr std::uint32_t kLocaleSISO639LangName = 0x00000059u;
constexpr std::uint32_t kLocaleSName = 0x0000005cu;
constexpr std::uint32_t kLocaleIDefaultAnsiCodePage = 0x00001004u;
constexpr std::uint32_t kLocaleReturnNumber = 0x20000000u;
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

    return finish("locale-ext");
}
