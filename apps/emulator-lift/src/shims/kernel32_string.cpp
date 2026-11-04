// emulator-lift · 码页与字符串族：wsprintfA（varargs）+ lstrlenA
//
// ★ 口径（plan §4 D6 / §7.1）：
//   · 本作是 CP932 路径 ⇒ 字符串**按字节**处理；需要宽字符时走 `runtime/cp932` 的入库码表
//     （⛔ 不用宿主 MultiByteToWideChar：三平台必须同一张表）。
//   · `wsprintfA` 是 **cdecl varargs**：参数从 `[esp+8]` 起逐个读，格式串决定读几个。
//     常用集 `%d %u %s %c %x %X %p %o` + 宽度/精度/`h/hh/l/ll/I64/w` 修饰。
//   · ⛔ 不支持 `%n`（写回指针）—— 直接响亮失败；未知格式也响亮失败（不许静默吞）。
//   · **越界**：Win32 的 wsprintf 没有容量参数，本实现设一个明确上限（kMaxOut = 4096 字节），
//     超出就 `shimAbort`（打印已生成长度）—— 这是"不静默写坏 guest 内存"的唯一机械护栏。
//     上限与理由写在 `data/lift/overrides.md`（可见偏差），不是默会。
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include "runtime/cp932.h"
#include "runtime/x86.h"
#include "shims/register.h"

namespace lift {
namespace {

constexpr std::size_t kMaxOut = 4096;  // wsprintfA 的输出上限（见文件头：无容量参数时的显式护栏）

struct Spec {
    bool left = false;
    bool zero = false;
    bool plus = false;
    bool space = false;
    bool alt = false;
    int width = 0;
    int prec = -1;
    enum Len { None, H, HH, L, LL, I64, W } len = None;
    char conv = 0;
};

std::string fmtUnsigned(std::uint64_t v, unsigned base, bool upper) {
    if (v == 0) return "0";
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    std::string s;
    while (v) {
        s.push_back(digits[v % base]);
        v /= base;
    }
    std::string r(s.rbegin(), s.rend());
    return r;
}

void appendPadded(std::string& out, const std::string& body, const Spec& sp, bool numeric) {
    const int body_len = static_cast<int>(body.size());
    const int width = sp.width;
    if (width <= body_len) {
        out += body;
        return;
    }
    const int pad = width - body_len;
    if (sp.left) {
        out += body;
        out.append(static_cast<std::size_t>(pad), ' ');
    } else if (sp.zero && numeric && sp.prec < 0) {
        // 零填充要在符号 / 前缀之后
        std::size_t prefix = 0;
        if (!body.empty() && (body[0] == '-' || body[0] == '+' || body[0] == ' ')) prefix = 1;
        else if (body.size() >= 2 && body[0] == '0' && (body[1] == 'x' || body[1] == 'X')) prefix = 2;
        out += body.substr(0, prefix);
        out.append(static_cast<std::size_t>(pad), '0');
        out += body.substr(prefix);
    } else {
        out.append(static_cast<std::size_t>(pad), ' ');
        out += body;
    }
}

// 读一个变参的整数（按长度修饰取 32 / 64 位；64 位占两个栈槽）
std::uint64_t readIntArg(ShimCall& call, int& next, Spec::Len len) {
    if (len == Spec::LL || len == Spec::I64) {
        const std::uint64_t lo = call.u32(next++);
        const std::uint64_t hi = call.u32(next++);
        return lo | (hi << 32);
    }
    return call.u32(next++);
}

std::string formatSigned(std::int64_t v, const Spec& sp, bool upper) {
    bool neg = v < 0;
    std::uint64_t mag = neg ? (0 - static_cast<std::uint64_t>(v)) : static_cast<std::uint64_t>(v);
    std::string body = fmtUnsigned(mag, 10, upper);
    if (sp.prec >= 0 && static_cast<int>(body.size()) < sp.prec) {
        body.insert(0, static_cast<std::size_t>(sp.prec - static_cast<int>(body.size())), '0');
    }
    if (neg) body.insert(body.begin(), '-');
    else if (sp.plus) body.insert(body.begin(), '+');
    else if (sp.space) body.insert(body.begin(), ' ');
    return body;
}

std::string formatUnsignedPrefixed(std::uint64_t v, const Spec& sp) {    if (sp.conv == 'u' || sp.conv == 'i') {
        std::string body = fmtUnsigned(v, 10, false);
        if (sp.prec >= 0 && static_cast<int>(body.size()) < sp.prec)
            body.insert(0, static_cast<std::size_t>(sp.prec - static_cast<int>(body.size())), '0');
        return body;
    }
    const unsigned base = sp.conv == 'o' ? 8u : 16u;
    const bool upper = sp.conv == 'X';
    std::string body = fmtUnsigned(v, base, upper);
    if (sp.prec >= 0 && static_cast<int>(body.size()) < sp.prec)
        body.insert(0, static_cast<std::size_t>(sp.prec - static_cast<int>(body.size())), '0');
    if (sp.alt && sp.conv != 'o' && v != 0) body.insert(0, upper ? "0X" : "0x");
    if (sp.alt && sp.conv == 'o' && (body.empty() || body[0] != '0')) body.insert(body.begin(), '0');
    return body;
}

// guest 内存 / 堆必须有装配（`shimAbort` 在 shims/register.h）
void requireMem(X86& cpu, const char* api) {
    if (!cpu.mem) shimAbort("kernel32", api, "cpu.mem 为空（Machine 没装配 guest 内存）");
}

}  // namespace

void shimWsprintfA(X86& cpu, ShimCall& call) {
    const std::uint32_t dst = call.u32(0);
    const std::uint32_t fmt_ea = call.u32(1);
    if (dst == 0 || fmt_ea == 0) shimAbort("user32", "wsprintfA", "lpOut/lpFmt 为 NULL（%08X / %08X）", dst, fmt_ea);

    const std::string fmt = cpu.mem->readCString(fmt_ea);
    std::string out;
    out.reserve(fmt.size() + 32);
    int next = 2;  // 变参从第 3 个参数起（0/1 是 lpOut / lpFmt）

    auto putc = [&](char c) {
        if (out.size() >= kMaxOut) {
            shimAbort("user32", "wsprintfA", "输出超过 %zu 字节上限（格式串过长 / 精度失控）", kMaxOut);
        }
        out.push_back(c);
    };
    auto puts = [&](const std::string& s) {
        for (const char c : s) putc(c);
    };

    for (std::size_t i = 0; i < fmt.size(); ++i) {
        const char c = fmt[i];
        if (c != '%') {
            putc(c);
            continue;
        }
        ++i;
        if (i >= fmt.size()) shimAbort("user32", "wsprintfA", "格式串以 %% 结尾");
        if (fmt[i] == '%') {
            putc('%');
            continue;
        }
        Spec sp;
        // flags
        for (; i < fmt.size(); ++i) {
            const char f = fmt[i];
            if (f == '-') sp.left = true;
            else if (f == '0') sp.zero = true;
            else if (f == '+') sp.plus = true;
            else if (f == ' ') sp.space = true;
            else if (f == '#') sp.alt = true;
            else break;
        }
        // width
        if (i < fmt.size() && fmt[i] == '*') {
            sp.width = call.i32(next++);
            if (sp.width < 0) {
                sp.left = true;
                sp.width = -sp.width;
            }
            ++i;
        } else {
            while (i < fmt.size() && fmt[i] >= '0' && fmt[i] <= '9') {
                sp.width = sp.width * 10 + (fmt[i] - '0');
                ++i;
            }
        }
        // precision
        if (i < fmt.size() && fmt[i] == '.') {
            ++i;
            sp.prec = 0;
            if (i < fmt.size() && fmt[i] == '*') {
                sp.prec = call.i32(next++);
                ++i;
            } else {
                while (i < fmt.size() && fmt[i] >= '0' && fmt[i] <= '9') {
                    sp.prec = sp.prec * 10 + (fmt[i] - '0');
                    ++i;
                }
            }
            if (sp.prec < 0) sp.prec = 0;
        }
        // length
        if (i + 1 < fmt.size() && fmt[i] == 'h' && fmt[i + 1] == 'h') {
            sp.len = Spec::HH;
            i += 2;
        } else if (i < fmt.size() && fmt[i] == 'h') {
            sp.len = Spec::H;
            ++i;
        } else if (i + 1 < fmt.size() && fmt[i] == 'l' && fmt[i + 1] == 'l') {
            sp.len = Spec::LL;
            i += 2;
        } else if (i < fmt.size() && fmt[i] == 'l') {
            sp.len = Spec::L;
            ++i;
        } else if (i + 2 < fmt.size() && fmt[i] == 'I' && fmt[i + 1] == '6' && fmt[i + 2] == '4') {
            sp.len = Spec::I64;
            i += 3;
        } else if (i < fmt.size() && fmt[i] == 'w') {
            sp.len = Spec::W;
            ++i;
        }
        if (i >= fmt.size()) shimAbort("user32", "wsprintfA", "格式串在长度修饰后结束");
        sp.conv = fmt[i];

        switch (sp.conv) {
            case 'd':
            case 'i': {
                std::int64_t v;
                if (sp.len == Spec::H) v = static_cast<std::int16_t>(call.u32(next++));
                else if (sp.len == Spec::HH) v = static_cast<std::int8_t>(call.u32(next++));
                else if (sp.len == Spec::LL || sp.len == Spec::I64)
                    v = static_cast<std::int64_t>(readIntArg(call, next, sp.len));
                else v = static_cast<std::int32_t>(call.u32(next++));
                const std::string body = formatSigned(v, sp, false);
                appendPadded(out, body, sp, true);
                break;
            }
            case 'u':
            case 'o':
            case 'x':
            case 'X': {
                std::uint64_t v = readIntArg(call, next, sp.len);
                if (sp.len == Spec::H) v &= 0xFFFFu;
                else if (sp.len == Spec::HH) v &= 0xFFu;
                else if (sp.len == Spec::None || sp.len == Spec::L || sp.len == Spec::W) v &= 0xFFFFFFFFu;
                const std::string body = formatUnsignedPrefixed(v, sp);
                appendPadded(out, body, sp, true);
                break;
            }
            case 'c':
            case 'C': {
                const std::uint32_t v = call.u32(next++);
                std::string s;
                if (sp.conv == 'C' || sp.len == Spec::W) {
                    std::uint8_t b[2] = {};
                    const int n = cp932Encode(static_cast<char16_t>(v), b);
                    if (n == 0) shimAbort("user32", "wsprintfA", "%%C 的码位 U+%04X 在 CP932 里不可编码", v);
                    s.assign(reinterpret_cast<const char*>(b), static_cast<std::size_t>(n));
                } else {
                    s.push_back(static_cast<char>(v & 0xFF));
                }
                appendPadded(out, s, sp, false);
                break;
            }
            case 's':
            case 'S': {
                const std::uint32_t ea = call.u32(next++);
                std::string s;
                if (!ea) {
                    s = "(null)";
                } else if (sp.conv == 'S' || sp.len == Spec::W) {
                    std::u16string w;
                    for (std::size_t k = 0; k < 65536; ++k) {
                        const char16_t ch = static_cast<char16_t>(cpu.mem->rd16(ea + static_cast<std::uint32_t>(k) * 2u));
                        if (!ch) break;
                        w.push_back(ch);
                    }
                    s = wideToCp932(w);
                } else {
                    for (std::size_t k = 0; k < 65536; ++k) {
                        const char ch = static_cast<char>(cpu.mem->rd8(ea + static_cast<std::uint32_t>(k)));
                        if (!ch) break;
                        s.push_back(ch);
                    }
                }
                if (sp.prec >= 0 && static_cast<int>(s.size()) > sp.prec) s.resize(static_cast<std::size_t>(sp.prec));
                appendPadded(out, s, sp, false);
                break;
            }
            case 'p': {
                const std::uint32_t v = call.u32(next++);
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%08X", v);
                appendPadded(out, buf, sp, true);
                break;
            }
            case 'n':
                shimAbort("user32", "wsprintfA", "%%n 不在支持集（写回指针 = 潜在越界）");
                break;
            default:
                shimAbort("user32", "wsprintfA", "不支持的格式 %%…%c（格式串：%s）", sp.conv, fmt.c_str());
        }
        if (out.size() > kMaxOut) {
            shimAbort("user32", "wsprintfA", "输出超过 %zu 字节上限", kMaxOut);
        }
    }

    if (!cpu.mem->committed(dst, static_cast<std::size_t>(out.size()) + 1)) {
        shimAbort("user32", "wsprintfA", "输出缓冲区未提交：ea=%08X len=%zu", dst, out.size() + 1);
    }
    cpu.mem->writeBytes(dst, out.data(), out.size());
    cpu.mem->wr8(dst + static_cast<std::uint32_t>(out.size()), 0);
    call.result = static_cast<std::uint32_t>(out.size());
}

// lstrlenA：字节长度（CP932 的多字节字符按字节计——与 Win32 语义一致）
void shimLstrlenA(X86& cpu, ShimCall& call) {
    const std::uint32_t ea = call.u32(0);
    if (!ea) {
        call.result = 0;
        return;
    }
    call.result = static_cast<std::uint32_t>(cpu.mem->readCString(ea).size());
}

// =======================================================================================
// GetLocaleInfoW / EnumSystemLocalesA（码页与字符串族的 locale 部分）
//
// ★ 口径：**表驱动**，只实现一张**固定**的 locale 表（en-US 0409 / ja-JP 0411）。
//   · `Locale` 接受 0409 / 0411，以及 LOCALE_USER_DEFAULT(0x0400) / LOCALE_SYSTEM_DEFAULT(0x0800)
//     （本模拟器只有一个用户 ⇒ 两者都映射到 0409）；其它 LCID ⇒ 0 + ERROR_INVALID_PARAMETER(87)。
//   · `LCType` 支持文档里常用的一批字符串 / 数值类型；**不支持的类型返回 0 + 87**
//     （⛔ 不返回空串 —— 那会让调用方把"没有这个值"当成"值是空"）。
//   · 返回值按文档：`cchData==0` ⇒ 所需大小（字符串含结尾 0）；成功 ⇒ 写入的字符数
//     （字符串**含**结尾 0）；`LOCALE_RETURN_NUMBER` ⇒ 2（Unicode 版是 `sizeof(DWORD)/sizeof(WCHAR)`）；
//     缓冲不够 ⇒ 0 + ERROR_INSUFFICIENT_BUFFER(122)。
//
// ⚠ 偏差：
//   1. **只有两个 locale**（上表）。真机有几百个；`EnumSystemLocalesA` 也只枚举这一张表
//      （⇒ 回调次数是确定的 2 次，且每个被枚举的 LCID 都能被 GetLocaleInfoW 回答）。
//   2. **locale 的值是 XP 年代的文档值**（日期/时间模式、分隔符、代码页…）；不做用户的区域设置覆写
//      （`SetLocaleInfo` 未导入）。
//   3. **`GetLocaleInfoA` 不在本文件**：它仍是 `kernel32_bootstrap.cpp` 里的 CRT 兼容 shim
//      （对所有 LCType 都回 "0409"）。本文件只做 W 版；两者的差异是**已知**的（A 版不按文档，
//      因为改它会影响 CRT 启动路径；留待后续批次统一）。
//   4. `LCID_ALTERNATE_SORTS` 不建模：只用它时按基础集合枚举并留一行日志（不假装有交替排序）。
// =======================================================================================
constexpr std::uint32_t kLcidEnUs = 0x0409u;
constexpr std::uint32_t kLcidJaJp = 0x0411u;
constexpr std::uint32_t kLcidUserDefault = 0x0400u;
constexpr std::uint32_t kLcidSystemDefault = 0x0800u;

constexpr std::uint32_t kErrInvalidParameter = 87u;
constexpr std::uint32_t kErrInsufficientBuffer = 122u;
constexpr std::uint32_t kErrInvalidFlags = 1008u;
constexpr std::uint32_t kErrNotEnoughMemory = 8u;

// LCType 常量（winnls.h 的数值；见 tools 侧的同一份文档）
constexpr std::uint32_t kLocaleILanguage = 0x00000001u;
constexpr std::uint32_t kLocaleSLocalizedDisplayName = 0x00000002u;
constexpr std::uint32_t kLocaleSAbbrevLangName = 0x00000003u;
constexpr std::uint32_t kLocaleSNativeLangName = 0x00000004u;
constexpr std::uint32_t kLocaleIDialingCode = 0x00000005u;
constexpr std::uint32_t kLocaleSAbbrevCtryName = 0x00000007u;
constexpr std::uint32_t kLocaleSNativeCtryName = 0x00000008u;
constexpr std::uint32_t kLocaleIDefaultCodePage = 0x0000000Bu;
constexpr std::uint32_t kLocaleSList = 0x0000000Cu;
constexpr std::uint32_t kLocaleIMeasure = 0x0000000Du;
constexpr std::uint32_t kLocaleSDecimal = 0x0000000Eu;
constexpr std::uint32_t kLocaleSThousand = 0x0000000Fu;
constexpr std::uint32_t kLocaleSGrouping = 0x00000010u;
constexpr std::uint32_t kLocaleIDigits = 0x00000011u;
constexpr std::uint32_t kLocaleILZero = 0x00000012u;
constexpr std::uint32_t kLocaleSNativeDigits = 0x00000013u;
constexpr std::uint32_t kLocaleSCurrency = 0x00000014u;
constexpr std::uint32_t kLocaleICurrDigits = 0x00000019u;
constexpr std::uint32_t kLocaleICurrency = 0x0000001Bu;
constexpr std::uint32_t kLocaleINegCurr = 0x0000001Cu;
constexpr std::uint32_t kLocaleSDate = 0x0000001Du;
constexpr std::uint32_t kLocaleSTime = 0x0000001Eu;
constexpr std::uint32_t kLocaleSShortDate = 0x0000001Fu;
constexpr std::uint32_t kLocaleSLongDate = 0x00000020u;
constexpr std::uint32_t kLocaleSAM = 0x00000028u;
constexpr std::uint32_t kLocaleSPM = 0x00000029u;
constexpr std::uint32_t kLocaleSISO639LangName = 0x00000059u;
constexpr std::uint32_t kLocaleSISO3166CtryName = 0x0000005Au;
constexpr std::uint32_t kLocaleSName = 0x0000005Cu;
constexpr std::uint32_t kLocaleSISO639LangName2 = 0x00000067u;
constexpr std::uint32_t kLocaleSISO3166CtryName2 = 0x00000068u;
constexpr std::uint32_t kLocaleSEnglishLangName = 0x00001001u;
constexpr std::uint32_t kLocaleSEnglishCtryName = 0x00001002u;
constexpr std::uint32_t kLocaleSTimeFormat = 0x00001003u;
constexpr std::uint32_t kLocaleIDefaultAnsiCodePage = 0x00001004u;
constexpr std::uint32_t kLocaleSYearMonth = 0x00001006u;
constexpr std::uint32_t kLocaleICalendarType = 0x00001009u;
constexpr std::uint32_t kLocaleINegNumber = 0x00001010u;
constexpr std::uint32_t kLocaleIDigitSubstitution = 0x00001014u;
constexpr std::uint32_t kLocaleReturnNumber = 0x20000000u;

struct LocaleDef {
    std::uint32_t lcid;
    const char16_t* sname;
    const char16_t* iso639;
    const char16_t* iso3166;
    const char16_t* iso639_3;
    const char16_t* iso3166_3;
    const char16_t* abbrev_lang;
    const char16_t* abbrev_country;
    const char16_t* disp_name;
    const char16_t* english_lang;
    const char16_t* native_lang;
    const char16_t* english_country;
    const char16_t* native_country;
    const char16_t* short_date;
    const char16_t* long_date;
    const char16_t* time_format;
    const char16_t* year_month;
    const char16_t* am;
    const char16_t* pm;
    const char16_t* decimal;
    const char16_t* thousand;
    const char16_t* list_sep;
    const char16_t* grouping;
    const char16_t* native_digits;
    const char16_t* currency;
    const char16_t* date_sep;
    const char16_t* time_sep;
    std::uint32_t ansi_cp;
    std::uint32_t oem_cp;
    std::uint32_t ilanguage;
    std::uint32_t dialing;
    std::uint32_t imeasure;
    std::uint32_t idigits;
    std::uint32_t ilzero;
    std::uint32_t icurrdigits;
    std::uint32_t icurrency;
    std::uint32_t inegcurr;
    std::uint32_t inegnumber;
    std::uint32_t icalendartype;
    std::uint32_t idigitsubstitution;
};

// ★ 表就是**枚举集合**：`EnumSystemLocalesA` 的 locale 列表由此派生（单一真源）。
const LocaleDef kLocales[] = {
    {kLcidEnUs,
     u"en-US", u"en", u"US", u"eng", u"USA", u"ENU", u"USA",
     u"English (United States)", u"English", u"English", u"United States", u"United States",
     u"M/d/yyyy", u"dddd, MMMM dd, yyyy", u"h:mm:ss tt", u"MMMM, yyyy", u"AM", u"PM",
     u".", u",", u",", u"3;0", u"0123456789", u"$", u"/", u":",
     1252u, 437u, kLcidEnUs, 1u, 1u, 2u, 1u, 2u, 0u, 0u, 1u, 1u, 0u},
    {kLcidJaJp,
     u"ja-JP", u"ja", u"JP", u"jpn", u"JPN", u"JPN", u"JPN",
     u"Japanese (Japan)", u"Japanese", u"日本語", u"Japan", u"日本",
     u"yy/MM/dd", u"yyyy'年'M'月'd'日'", u"H:mm:ss", u"yyyy'年'M'月'", u"午前", u"午後",
     u".", u",", u",", u"3;0", u"0123456789", u"\u00A5", u"/", u":",
     932u, 932u, kLcidJaJp, 81u, 1u, 2u, 1u, 0u, 0u, 0u, 1u, 1u, 0u},
};

const LocaleDef* findLocale(std::uint32_t lcid) {
    if (lcid == kLcidUserDefault || lcid == kLcidSystemDefault) lcid = kLcidEnUs;
    for (const LocaleDef& d : kLocales) {
        if (d.lcid == lcid) return &d;
    }
    return nullptr;
}

struct LocaleValue {
    bool is_number = false;
    std::uint32_t number = 0;
    std::u16string text;
};

bool lookupLocaleInfo(const LocaleDef& d, std::uint32_t type, LocaleValue* out) {
    auto text = [out](const char16_t* s) {
        out->is_number = false;
        out->text = s ? std::u16string(s) : std::u16string();
        return true;
    };
    auto number = [out](std::uint32_t v) {
        out->is_number = true;
        out->number = v;
        return true;
    };
    switch (type) {
        case kLocaleSName: return text(d.sname);
        case kLocaleSISO639LangName: return text(d.iso639);
        case kLocaleSISO3166CtryName: return text(d.iso3166);
        case kLocaleSISO639LangName2: return text(d.iso639_3);
        case kLocaleSISO3166CtryName2: return text(d.iso3166_3);
        case kLocaleSAbbrevLangName: return text(d.abbrev_lang);
        case kLocaleSAbbrevCtryName: return text(d.abbrev_country);
        case kLocaleSLocalizedDisplayName: return text(d.disp_name);
        case kLocaleSEnglishLangName: return text(d.english_lang);
        case kLocaleSNativeLangName: return text(d.native_lang);
        case kLocaleSEnglishCtryName: return text(d.english_country);
        case kLocaleSNativeCtryName: return text(d.native_country);
        case kLocaleSShortDate: return text(d.short_date);
        case kLocaleSLongDate: return text(d.long_date);
        case kLocaleSTimeFormat: return text(d.time_format);
        case kLocaleSYearMonth: return text(d.year_month);
        case kLocaleSAM: return text(d.am);
        case kLocaleSPM: return text(d.pm);
        case kLocaleSDecimal: return text(d.decimal);
        case kLocaleSThousand: return text(d.thousand);
        case kLocaleSList: return text(d.list_sep);
        case kLocaleSGrouping: return text(d.grouping);
        case kLocaleSNativeDigits: return text(d.native_digits);
        case kLocaleSCurrency: return text(d.currency);
        case kLocaleSDate: return text(d.date_sep);
        case kLocaleSTime: return text(d.time_sep);
        case kLocaleILanguage: return number(d.ilanguage);
        case kLocaleIDialingCode: return number(d.dialing);
        case kLocaleIDefaultCodePage: return number(d.oem_cp);
        case kLocaleIDefaultAnsiCodePage: return number(d.ansi_cp);
        case kLocaleIMeasure: return number(d.imeasure);
        case kLocaleIDigits: return number(d.idigits);
        case kLocaleILZero: return number(d.ilzero);
        case kLocaleICurrDigits: return number(d.icurrdigits);
        case kLocaleICurrency: return number(d.icurrency);
        case kLocaleINegCurr: return number(d.inegcurr);
        case kLocaleINegNumber: return number(d.inegnumber);
        case kLocaleICalendarType: return number(d.icalendartype);
        case kLocaleIDigitSubstitution: return number(d.idigitsubstitution);
        default: return false;
    }
}

void shimGetLocaleInfoW(X86& cpu, ShimCall& call) {
    const char* api = "GetLocaleInfoW";
    requireMem(cpu, api);
    std::uint32_t lcid = call.u32(0);
    std::uint32_t type = call.u32(1);
    const std::uint32_t buf = call.u32(2);
    const std::uint32_t cap = call.u32(3);
    const bool want_number = (type & kLocaleReturnNumber) != 0;
    type &= ~kLocaleReturnNumber;
    if ((type & 0xFF000000u) != 0) {  // 未知高位 flag
        cpu.shim.last_error = kErrInvalidFlags;
        shimLog("kernel32", api, "LCType=0x%08X 带未知 flag ⇒ 返回 0 + 错误 1008", type);
        call.result = 0;
        return;
    }
    const LocaleDef* def = findLocale(lcid);
    LocaleValue v;
    if (!def || !lookupLocaleInfo(*def, type, &v)) {
        cpu.shim.last_error = kErrInvalidParameter;
        shimLog("kernel32", api, "LCID=0x%04X / LCType=0x%08X 不在本模拟器的 locale 表里 ⇒ 返回 0 + 错误 87",
                lcid, type);
        call.result = 0;
        return;
    }
    if (want_number) {
        if (!v.is_number) {
            cpu.shim.last_error = kErrInvalidParameter;
            shimLog("kernel32", api, "LCType=0x%08X 是字符串类型，不能配 LOCALE_RETURN_NUMBER ⇒ 返回 0 + 错误 87",
                    type);
            call.result = 0;
            return;
        }
        constexpr std::uint32_t kNumberChars = sizeof(std::uint32_t) / sizeof(char16_t);  // = 2
        if (cap == 0) {
            call.result = kNumberChars;  // 所需大小（数值没有结尾 0）
            return;
        }
        if (buf == 0) {
            cpu.shim.last_error = kErrInvalidParameter;
            shimLog("kernel32", api, "lpLCData 为 NULL 但 cchData=%u ⇒ 返回 0 + 错误 87", cap);
            call.result = 0;
            return;
        }
        if (cap < kNumberChars) {
            cpu.shim.last_error = kErrInsufficientBuffer;
            shimLog("kernel32", api, "cchData=%u 小于数值所需的 %u ⇒ 返回 0 + 错误 122", cap,
                    kNumberChars);
            call.result = 0;
            return;
        }
        requireMem(cpu, api);
        cpu.mem->wr32(buf, v.number);
        call.result = kNumberChars;
        return;
    }

    const std::uint32_t need = static_cast<std::uint32_t>(v.text.size()) + 1u;
    if (cap == 0) {
        call.result = need;  // 文档：所需大小（含结尾 0）
        return;
    }
    if (buf == 0) {
        cpu.shim.last_error = kErrInvalidParameter;
        shimLog("kernel32", api, "lpLCData 为 NULL 但 cchData=%u ⇒ 返回 0 + 错误 87", cap);
        call.result = 0;
        return;
    }
    if (cap < need) {
        cpu.shim.last_error = kErrInsufficientBuffer;
        shimLog("kernel32", api, "cchData=%u 小于所需的 %u（含结尾 0）⇒ 返回 0 + 错误 122", cap, need);
        call.result = 0;
        return;
    }
    for (std::size_t i = 0; i < v.text.size(); ++i) {
        cpu.mem->wr16(buf + static_cast<std::uint32_t>(i) * 2u, v.text[i]);
    }
    cpu.mem->wr16(buf + static_cast<std::uint32_t>(v.text.size()) * 2u, 0);
    call.result = need;  // 文档：写入的字符数（含结尾 0）
}

// ---------------------------------------------------------------------------------------
// guest 回调的续延桩（`EnumSystemLocalesA` 用；机制见 x86.h 的 GuestCallbackState）
// ---------------------------------------------------------------------------------------
// ★ 固定落点：桩页是 0x7FFE0000..0x7FFE7FFF（0x2000 槽 × 4 字节），栈顶在 0x70000000
//   ⇒ 0x7FFEF000 不被任何区占用，也不会与 guest 的镜像 / 堆 / 栈重叠。
constexpr std::uint32_t kCallbackContinuationEa = 0x7FFEF000u;
// locale 串的**固定**暂存页（1 页；不借 guest 堆 —— 那样纯档夹具还必须先初始化堆，
// 而这里要的只是"一块确定的 guest 内存"）。0x7FFFD000 在 TEB/PEB/TLS/argv/env 之外、桩页之下。
constexpr std::uint32_t kLocaleScratchEa = 0x7FFFD000u;
constexpr std::uint32_t kLocaleScratchSize = 0x1000u;
// 内部 shim 的注册名（**不是导入**：IAT 里没有它，只有本文件的续延桩会用到）
constexpr const char* kCallbackContinuationName = "__guest_callback_return";

// 在 guest 栈上铺一帧"回调调用"，把 eip 交给 guest 回调：
//   [esp]   = 续延桩地址（回调的返回地址）
//   [esp+4] = 第 0 个参数
void startGuestCallback(X86& cpu, ShimCall& call) {
    GuestCallbackState& st = cpu.shim.callback;
    if (!st.active || st.next >= st.args.size()) {
        shimAbort("lift", kCallbackContinuationName, "续延状态非法（active=%d next=%zu size=%zu）",
                  st.active ? 1 : 0, st.next, st.args.size());
    }
    // 回调的 stdcall 帧必须落在已提交的栈里（base = 原始调用点的 esp）
    if (st.saved_esp < 8u || !cpu.mem->committed(st.saved_esp - 8u, 8u)) {
        shimAbort("lift", st.api ? st.api : "callback", "guest 栈不够铺回调帧：saved_esp=0x%08X",
                  st.saved_esp);
    }
    cpu.esp() = st.saved_esp - 8u;
    cpu.mem->wr32(st.saved_esp - 8u, kCallbackContinuationEa);
    cpu.mem->wr32(st.saved_esp - 4u, st.args[st.next]);
    cpu.eip = st.callback_ea;
    call.take_control = true;  // ⛔ 别让 invoke() 再弹参 / 改 eip
}

// 结束枚举：恢复原始 esp（含原本由 invoke() 负责的弹参）与返回地址
void finishGuestCallback(X86& cpu, ShimCall& call, std::uint32_t result) {
    GuestCallbackState& st = cpu.shim.callback;
    cpu.esp() = st.saved_esp + st.cleanup_bytes;
    cpu.eip = st.ret_ea;
    st.active = false;
    st.args.clear();
    st.next = 0;
    st.scratch_ea = 0;
    st.scratch_len = 0;
    call.take_control = true;
    call.result = result;
}

void shimGuestCallbackReturn(X86& cpu, ShimCall& call) {
    GuestCallbackState& st = cpu.shim.callback;
    if (!st.active) {
        shimAbort("lift", kCallbackContinuationName,
                  "续延桩被调到，但没有活动的 guest 回调 —— guest 跳到了 0x%08X？",
                  kCallbackContinuationEa);
    }
    // 回调的返回值就是 EAX（BOOL CALLBACK EnumLocalesProcA(LPSTR)）
    const std::uint32_t callback_result = cpu.eax();
    // 先恢复原始栈：下一帧回调 / 收尾都以它为基准
    cpu.esp() = st.saved_esp;
    ++st.next;
    if (callback_result == 0) {
        // 文档：回调返回 FALSE ⇒ 枚举停止（后续回调不再发生）
        shimLog("kernel32", st.api ? st.api : "EnumSystemLocalesA",
                "guest 回调返回 FALSE ⇒ 停止枚举（已回调 %zu / %zu 次）", st.next, st.args.size());
        finishGuestCallback(cpu, call, 1);
        return;
    }
    if (st.next >= st.args.size()) {
        finishGuestCallback(cpu, call, 1);  // 全部回调完成 ⇒ TRUE
        return;
    }
    startGuestCallback(cpu, call);
}

// ---- EnumSystemLocalesA ----
// 文档：`lpLocaleEnumProc` 对每个 locale 调一次（参数是 8 位十六进制 LCID 串），回调返回 FALSE
// 即停止；`dwFlags` 为 0 等价于 LCID_SUPPORTED。本模拟器枚举**固定的 locale 表**（2 条，见上）。
void shimEnumSystemLocalesA(X86& cpu, ShimCall& call) {
    const char* api = "EnumSystemLocalesA";
    requireMem(cpu, api);
    constexpr std::uint32_t kLcidInstalled = 0x00000001u;
    constexpr std::uint32_t kLcidSupported = 0x00000002u;
    constexpr std::uint32_t kLcidAlternateSorts = 0x00000004u;
    const std::uint32_t callback = call.u32(0);
    const std::uint32_t flags = call.u32(1);
    if (callback == 0) {
        cpu.shim.last_error = kErrInvalidParameter;
        shimLog("kernel32", api, "lpLocaleEnumProc 为 NULL ⇒ 返回 0 + 错误 87");
        call.result = 0;
        return;
    }
    if ((flags & ~(kLcidInstalled | kLcidSupported | kLcidAlternateSorts)) != 0 ||
        ((flags & kLcidInstalled) && (flags & kLcidSupported))) {
        cpu.shim.last_error = kErrInvalidFlags;
        shimLog("kernel32", api, "dwFlags=0x%X 非法（INSTALLED 与 SUPPORTED 互斥；未知位不接受）⇒ 返回 0 + 错误 1008",
                flags);
        call.result = 0;
        return;
    }
    if ((flags & kLcidAlternateSorts) != 0) {
        shimLog("kernel32", api, "LCID_ALTERNATE_SORTS 不建模 ⇒ 只枚举基础集合");
    }
    GuestCallbackState& st = cpu.shim.callback;
    if (st.active) {
        shimAbort("kernel32", api,
                  "上一次 guest 回调没有返回（active=true, api=%s）—— 不允许重入枚举",
                  st.api ? st.api : "?");
    }
    if (!cpu.cache) shimAbort("kernel32", api, "cpu.cache 为空（Machine 没装配块缓存）");
    ShimDef* cont = cpu.shims ? cpu.shims->findMutable("lift", kCallbackContinuationName) : nullptr;
    if (!cont) {
        shimAbort("kernel32", api,
                  "内部续延 shim `lift!%s` 没注册（registerStringShims 未跑？）",
                  kCallbackContinuationName);
    }
    cpu.cache->installShim(kCallbackContinuationEa, cont->id);

    // locale 串放进**固定暂存页**（每条 16 字节对齐；8 位十六进制 + 结尾 0 = 9 字节）
    const std::uint32_t count = static_cast<std::uint32_t>(sizeof(kLocales) / sizeof(kLocales[0]));
    const std::uint32_t stride = 16u;
    if (count * stride > kLocaleScratchSize) {
        shimAbort("kernel32", api, "locale 暂存页不够：需要 %u 字节，只有 %u", count * stride,
                  kLocaleScratchSize);
    }
    if (!cpu.mem->committed(kLocaleScratchEa, kLocaleScratchSize)) {
        if (!cpu.mem->commitZeroed(kLocaleScratchEa, kLocaleScratchSize, RegionKind::Other,
                                   "locale-scratch")) {
            cpu.shim.last_error = kErrNotEnoughMemory;
            shimLog("kernel32", api, "提交 locale 暂存页 0x%08X 失败 ⇒ 返回 0 + 错误 8",
                    kLocaleScratchEa);
            call.result = 0;
            return;
        }
    }
    const std::uint32_t scratch = kLocaleScratchEa;
    st.active = true;
    st.ret_ea = call.ret_ea;
    st.saved_esp = cpu.esp();
    st.cleanup_bytes = 4u + static_cast<std::uint32_t>(call.argc) * 4u;  // 原本 invoke() 会弹的那些
    st.callback_ea = callback;
    st.scratch_ea = scratch;
    st.scratch_len = count * stride;
    st.args.clear();
    st.next = 0;
    st.api = api;
    static const char16_t kHex[] = u"0123456789ABCDEF";
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t ea = scratch + i * stride;
        for (int k = 0; k < 8; ++k) {
            const std::uint32_t nibble = (kLocales[i].lcid >> (4 * (7 - k))) & 0xFu;
            cpu.mem->wr8(ea + static_cast<std::uint32_t>(k), static_cast<std::uint8_t>(kHex[nibble]));
        }
        cpu.mem->wr8(ea + 8u, 0);
        st.args.push_back(ea);
    }
    startGuestCallback(cpu, call);
}

void registerStringShims(ShimRegistry& s) {
    // ★ wsprintfA 是 varargs：声明 argc=2（仅用于 trace / 诊断），实际参数个数由格式串决定。
    s.add("user32", "wsprintfA", Conv::Varargs, 2, &shimWsprintfA);
    s.add("kernel32", "lstrlenA", Conv::Stdcall, 1, &shimLstrlenA);
    s.add("kernel32", "GetLocaleInfoW", Conv::Stdcall, 4, &shimGetLocaleInfoW);
    s.add("kernel32", "EnumSystemLocalesA", Conv::Stdcall, 2, &shimEnumSystemLocalesA);
    // ★ 内部续延桩（**不是导入**：不进 IAT、不进 overrides；只有 EnumSystemLocalesA 会用它）
    s.add("lift", kCallbackContinuationName, Conv::Stdcall, 0, &shimGuestCallbackReturn);
}

}  // namespace lift
