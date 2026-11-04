// emulator-lift · 字符串 / 码页（pure：自造字节块，不碰游戏）
//
// 判据：
// ① `wsprintfA`（cdecl varargs）的常用格式集：`%d %u %x %X %s %c %p` + 宽度/精度/`h/hh/l/ll/I64`；
//    返回长度 = 写出的字节数（不含结尾 0），且**正好**写到缓冲区（多 1 字节都不行）。
// ② CP932 表：定点映射（0x82A0=あ / 0x835C=ソ / 半角片假名）+ 8 个单字节例外 + 不可解/不可编码。
// ③ watchdog：`%n` 必须响亮失败（它是"写回指针 = 潜在越界"）。
// ★ 用 guest 机器码走真实 shim 派发路径（不是直接调 C++ 函数）—— 这才顺带验了 varargs 的栈布局。
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "runtime/cp932.h"
#include "runtime/machine.h"
#include "shims/register.h"

namespace {

constexpr std::uint32_t kCodeEa = 0x00401000u;
constexpr std::uint32_t kDataEa = 0x00402000u;
constexpr std::uint32_t kStubEa = 0x7FFE0100u;

struct Asm {
    std::vector<std::uint8_t> b;
    std::uint32_t currentEa() const { return kCodeEa + static_cast<std::uint32_t>(b.size()); }
    void u8(std::uint8_t v) { b.push_back(v); }
    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
    }
    void pushImm(std::uint32_t v) {
        u8(0x68);
        u32(v);
    }
    void callRel(std::uint32_t target) {
        const auto at = static_cast<std::int32_t>(currentEa());
        const auto rel = static_cast<std::int32_t>(target) - (at + 5);
        u8(0xE8);
        u32(static_cast<std::uint32_t>(rel));
    }
    void addEspImm(std::uint32_t v) {
        u8(0x81);
        u8(0xC4);
        u32(v);
    }
    void ret() { u8(0xC3); }
};

struct Fixture {
    lift::Machine m;
    std::string err;
    std::uint32_t data_cursor = kDataEa;
    bool ok = true;

    Fixture() {
        ok = m.initBare(&err) && m.prepareStack(&err);
        if (!ok) return;
        lift::registerStringShims(m.shims());
        lift::ShimDef* def = m.shims().findMutable("user32", "wsprintfA");
        if (!def) {
            ok = false;
            err = "wsprintfA 没注册";
            return;
        }
        def->stub_ea = kStubEa;
        m.cache().installShim(kStubEa, def->id);
    }

    std::uint32_t put(const void* p, std::size_t n) {
        const std::uint32_t ea = data_cursor;
        data_cursor = (data_cursor + static_cast<std::uint32_t>(n) + 15u) & ~15u;
        ok = ok && m.placeData(ea, p, n, &err);
        return ea;
    }
    std::uint32_t cstr(const std::string& s) { return put(s.c_str(), s.size() + 1); }
    std::uint32_t wstr(const std::u16string& s) { return put(s.data(), (s.size() + 1) * 2); }
    std::uint32_t zeroBuf(std::size_t n = 512) {
        std::vector<std::uint8_t> z(n, 0);
        return put(z.data(), z.size());
    }

    std::uint32_t call(const std::vector<std::uint32_t>& args) {
        Asm a;
        for (auto it = args.rbegin(); it != args.rend(); ++it) a.pushImm(*it);
        a.callRel(kStubEa);
        a.addEspImm(static_cast<std::uint32_t>(args.size()) * 4u);
        a.ret();
        ok = ok && m.placeCode(kCodeEa, a.b.data(), a.b.size(), &err);
        m.cpu().eip = kCodeEa;
        if (!ok) return 0;
        lift::RunOptions o;
        o.until_ea = 0xFFFFFFFFu;
        o.max_insns = 10000;
        const lift::RunResult r = m.run(o);
        LIFT_CHECK(r.reason == lift::StopReason::UntilEa);
        return m.cpu().eax();
    }
};

void expect(Fixture& f, std::uint32_t buf, const std::string& want, const char* label) {
    const std::string got = f.m.mem().readCString(buf);
    if (got != want) {
        std::fprintf(stderr, "[%s] wsprintfA: got \"%s\" want \"%s\"\n", label, got.c_str(), want.c_str());
    }
    LIFT_CHECK(got == want);
}

}  // namespace

int main() {
    using namespace lift_test;

    // ---------------- wsprintfA：常用集 + 宽度 / 精度 / 长度 ----------------
    {
        g_current_case = "wsprintf-basic";
        Fixture f;
        LIFT_CHECK(f.ok);
        const std::uint32_t buf = f.zeroBuf();
        const std::uint32_t fmt = f.cstr("%d/%u/%x/%X/%s/%c/%p");
        const std::uint32_t s = f.cstr("AB");
        const std::uint32_t n = f.call({buf, fmt, static_cast<std::uint32_t>(-42), 7u, 0xBEEFu, 0xBEEFu, s,
                                        static_cast<std::uint32_t>('Z'), 0x00401234u});
        expect(f, buf, "-42/7/beef/BEEF/AB/Z/00401234", "basic");
        LIFT_CHECK_EQ_U32(n, 29u);
    }

    {
        g_current_case = "wsprintf-width-precision";
        Fixture f;
        LIFT_CHECK(f.ok);
        const std::uint32_t buf = f.zeroBuf();
        const std::uint32_t fmt = f.cstr("|%08X|%-5d|%+d|%.3d|%I64u|%hd|%hhx|");
        const std::uint32_t n =
            f.call({buf, fmt, 0xBEu, 7u, 5u, 7u, 0x0000000100000002ull & 0xFFFFFFFFu, 0x0000000100000002ull >> 32,
                    0x12345678u, 0x1FFu});
        expect(f, buf, "|000000BE|7    |+5|007|4294967298|22136|ff|", "width");
        LIFT_CHECK_EQ_U32(n, static_cast<std::uint32_t>(std::string("|000000BE|7    |+5|007|4294967298|22136|ff|").size()));
    }

    {
        g_current_case = "wsprintf-cp932-bytes";
        Fixture f;
        LIFT_CHECK(f.ok);
        const std::uint32_t buf = f.zeroBuf();
        const std::uint32_t fmt = f.cstr("<%s>");
        const std::string a = "\x82\xA0";  // あ（CP932 双字节）
        const std::uint32_t s = f.cstr(a);
        const std::uint32_t n = f.call({buf, fmt, s});
        expect(f, buf, "<\x82\xA0>", "cp932-bytes");
        LIFT_CHECK_EQ_U32(n, 4u);
    }

    {
        g_current_case = "wsprintf-wide-to-cp932";
        Fixture f;
        LIFT_CHECK(f.ok);
        const std::uint32_t buf = f.zeroBuf();
        const std::uint32_t fmt = f.cstr("%S");
        const std::uint32_t s = f.wstr(u"\u3042\u30BD");  // あソ
        const std::uint32_t n = f.call({buf, fmt, s});
        expect(f, buf, "\x82\xA0\x83\x5C", "wide");
        LIFT_CHECK_EQ_U32(n, 4u);
    }

    // ---------------- CP932 表（定点 + 边界）----------------
    {
        g_current_case = "cp932-table";
        char16_t u = 0;
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(lift::cp932Decode(reinterpret_cast<const std::uint8_t*>("\x82\xA0"), 2, &u)), 2u);
        LIFT_CHECK_EQ_U32(u, 0x3042u);  // あ
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(lift::cp932Decode(reinterpret_cast<const std::uint8_t*>("\x83\x5C"), 2, &u)), 2u);
        LIFT_CHECK_EQ_U32(u, 0x30BDu);  // ソ
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(lift::cp932Decode(reinterpret_cast<const std::uint8_t*>("\xB1"), 1, &u)), 1u);
        LIFT_CHECK_EQ_U32(u, 0xFF71u);  // ｱ（半角片假名）
        // 8 个单字节例外（Windows CP932 与 WHATWG shift_jis 的差异）
        const std::uint16_t special[8][2] = {{0x1A, 0x001A}, {0x1C, 0x001C}, {0x7F, 0x007F}, {0x80, 0x0080},
                                            {0xA0, 0xF8F0}, {0xFD, 0xF8F1}, {0xFE, 0xF8F2}, {0xFF, 0xF8F3}};
        for (const auto& row : special) {
            const std::uint8_t b = static_cast<std::uint8_t>(row[0]);
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(lift::cp932Decode(&b, 1, &u)), 1u);
            LIFT_CHECK_EQ_U32(u, row[1]);
        }
        // 未定义的双字节（CP932 里 0x81 0x20 不是合法 pair）
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(lift::cp932Decode(reinterpret_cast<const std::uint8_t*>("\x81\x20"), 2, &u)), 0u);
        // 编码：往返 + 不可编码 ⇒ '?'
        std::uint8_t out[2] = {};
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(lift::cp932Encode(0x3042, out)), 2u);
        LIFT_CHECK_EQ_U32(out[0], 0x82u);
        LIFT_CHECK_EQ_U32(out[1], 0xA0u);
        std::size_t bad = 0;
        const std::string enc = lift::wideToCp932(u"あ\u2603", &bad);  // U+2603 雪花不在 CP932
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(enc.size()), 3u);
        LIFT_CHECK_EQ_U32(bad, 1u);
        LIFT_CHECK(enc == "\x82\xA0?");
    }

    return finish("strfmt");
}
