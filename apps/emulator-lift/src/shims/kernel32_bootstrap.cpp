// emulator-lift · CRT 启动路径上的补充 shim（S7 的"缺口填平"）
//
// ★ 为什么单独一个文件：S6 的**前三族**（同步 / 内存 / 错误码与 TLS）在
//   `kernel32_{sync,mem,error_tls}.cpp`；这里放的是**只有跑启动路径才暴露出来**的那些
//   （`_heap_init` / `_ioinit` / `_setenvp` / `_setlocale` 一路会碰的 API），
//   以及 S6 后四族里被启动路径**实际用到**的最小一块（进程/模块、计时、码页）。
//   ⛔ 它们不是 stub：每条都按 Win32 的真实语义实现（做不到的点在注释与本文件末尾点名）。
// ★ 码页：本作是 CP932；真表在 `runtime/cp932`（S6 码页族，自带表、三平台同一份）。
//   932 / 65001 走真表；其余码页退回"ASCII 正确 + 非 ASCII 字节直通"并记一次日志（显式缺口）。
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "runtime/cp932.h"
#include "runtime/machine.h"
#include "runtime/sha256.h"
#include "runtime/x86.h"
#include "shims/register.h"

namespace lift {
namespace {

std::uint32_t guestWrite(GuestMemory& mem, std::uint32_t ea, const std::string& s) {
    if (ea) mem.writeBytes(ea, s.c_str(), s.size() + 1);
    return static_cast<std::uint32_t>(s.size());
}

// ---------------------------------------------------------------------------------------
// 堆的"信息类" API（`_heap_init` 会碰 HeapSetInformation）
// ---------------------------------------------------------------------------------------
void shimHeapSetInformation(X86&, ShimCall& call) {
    // HeapCompatibilityInformation(0) = 关掉 LFH；我们本来就没有 LFH ⇒ 成功。
    const std::uint32_t info_class = call.u32(1);
    if (info_class != 0) {
        shimLog("kernel32", "HeapSetInformation", "只支持 HeapCompatibilityInformation(0)，其余按失败返回");
        call.result = 0;
        return;
    }
    call.result = 1;
}

void shimHeapQueryInformation(X86&, ShimCall& call) {
    // 5 个参数；不往 guest 里写任何东西（没实现任何信息类）⇒ 失败 + 错误码
    (void)call;
    call.result = 0;
    call.cpu->shim.last_error = 87;  // ERROR_INVALID_PARAMETER
    shimLog("kernel32", "HeapQueryInformation", "没有实现任何堆信息类（M1a 不需要）");
}

void shimHeapValidate(X86&, ShimCall& call) { call.result = 1; }  // 我们的堆只有一份、且操作即时校验
void shimHeapCompact(X86&, ShimCall& call) { call.result = 0; }   // 没有可压缩的（无 LFH）
void shimHeapLock(X86&, ShimCall& call) { call.result = 1; }
void shimHeapUnlock(X86&, ShimCall& call) { call.result = 1; }

void shimGetProcessHeaps(X86& cpu, ShimCall& call) {
    const std::uint32_t count = call.u32(0);
    const std::uint32_t arr = call.u32(1);
    if (arr && count >= 1) cpu.mem->wr32(arr, cpu.shim.heap_handle);
    call.result = 1;
}

// ---------------------------------------------------------------------------------------
// 标准句柄 / 文件类型（`_ioinit` 会碰）
//
// ★ 2026-11-04 收口 REQ-01M6K69K9HM3ETCDNMJWPHGXJC：这三条此前是**互相矛盾的空实现**
//   （`SetStdHandle` 不存值 / `GetFileType` 恒报字符设备 / `GetConsoleMode` 恒成功）。
//   现在三者读的是**同一份事实**：
//     · `SetStdHandle` 存进句柄表的进程级标准槽（`HandleTable::setStdHandle`），`GetStdHandle` 读回；
//     · `GetFileType` 先看句柄表里的**真实种类**（File ⇒ FILE_TYPE_DISK(1)），再看标准槽
//       （默认的三个假句柄 ⇒ FILE_TYPE_CHAR(2)），其余 ⇒ FILE_TYPE_UNKNOWN(0) + 错误；
//     · `GetConsoleMode` 只对**标准槽里的假句柄**成功（启动路径的现状），真句柄 ⇒ 0 + 6。
// ⛔ 边界（本单不做）：管道 / 真控制台设备；只要求"文件句柄 ≠ 字符设备"与"标准槽真的存值"。
// ---------------------------------------------------------------------------------------
constexpr std::uint32_t kFileTypeUnknown = 0;  // FILE_TYPE_UNKNOWN
constexpr std::uint32_t kFileTypeDisk = 1;     // FILE_TYPE_DISK
constexpr std::uint32_t kFileTypeChar = 2;     // FILE_TYPE_CHAR
constexpr std::uint32_t kErrInvalidHandle = 6;
constexpr std::uint32_t kErrInvalidParameter = 87;

// STD_INPUT(-10) / STD_OUTPUT(-11) / STD_ERROR(-12) ⇒ 标准槽号；其余 ⇒ -1
int stdSlotFromNStdHandle(std::uint32_t which) {
    switch (which) {
        case 0xFFFFFFF6u: return 0;  // STD_INPUT_HANDLE
        case 0xFFFFFFF5u: return 1;  // STD_OUTPUT_HANDLE
        case 0xFFFFFFF4u: return 2;  // STD_ERROR_HANDLE
        default: return -1;
    }
}

void shimGetStdHandle(X86& cpu, ShimCall& call) {
    const std::uint32_t which = call.u32(0);
    const int slot = stdSlotFromNStdHandle(which);
    if (slot < 0 || !cpu.handles) {
        cpu.shim.last_error = kErrInvalidHandle;
        shimLog("kernel32", "GetStdHandle", "nStdHandle=0x%08X 不是 STD_INPUT/OUTPUT/ERROR ⇒ 返回 0",
                which);
        call.result = 0;
        return;
    }
    call.result = cpu.handles->stdHandle(slot);
}

// 文档：改的是**进程级**表（之后 `GetStdHandle` 返回它）。h 可以为 0（清掉该槽）。
void shimSetStdHandle(X86& cpu, ShimCall& call) {
    const std::uint32_t which = call.u32(0);
    const std::uint32_t h = call.u32(1);
    const int slot = stdSlotFromNStdHandle(which);
    if (slot < 0 || !cpu.handles) {
        cpu.shim.last_error = kErrInvalidParameter;
        shimLog("kernel32", "SetStdHandle", "nStdHandle=0x%08X 不是 STD_INPUT/OUTPUT/ERROR ⇒ 返回 0",
                which);
        call.result = 0;
        return;
    }
    cpu.handles->setStdHandle(slot, h);
    call.result = 1;
}

// 文档（GetFileType）：按**句柄的真实种类**回答 —— 磁盘文件 FILE_TYPE_DISK(1)、字符设备
// FILE_TYPE_CHAR(2)、管道 FILE_TYPE_PIPE(3)、其余 / 无效句柄 FILE_TYPE_UNKNOWN(0)。
// ★ 旧实现恒返回 2 ⇒ CRT 会把 `CreateFileA` 打开的文件当控制台（缓冲 / 定位走错分支）。
void shimGetFileType(X86& cpu, ShimCall& call) {
    const std::uint32_t h = call.u32(0);
    if (cpu.handles) {
        // ① 真句柄：按句柄表的种类回答
        if (const HandleEntry* e = cpu.handles->get(h)) {
            if (e->kind == HandleKind::File) {
                call.result = kFileTypeDisk;
                return;
            }
            // 事件 / 互斥量 / 查找句柄…都不是文件对象：文档的失败值就是 FILE_TYPE_UNKNOWN(0)
            shimLog("kernel32", "GetFileType", "0x%08X 是 %s 句柄（不是文件对象）⇒ FILE_TYPE_UNKNOWN",
                    h, handleKindName(e->kind));
            call.result = kFileTypeUnknown;
            return;
        }
        // ② 标准槽里的假句柄（默认 0x0C/0x10/0x14，或 SetStdHandle 存进来的）：字符设备
        if (cpu.handles->stdSlotOf(h) >= 0) {
            call.result = kFileTypeChar;
            return;
        }
    }
    // ③ 其余 = 无效句柄 ⇒ FILE_TYPE_UNKNOWN + ERROR_INVALID_HANDLE
    cpu.shim.last_error = kErrInvalidHandle;
    shimLog("kernel32", "GetFileType", "0x%08X 不是活着的句柄 ⇒ FILE_TYPE_UNKNOWN(0) + 错误 6", h);
    call.result = kFileTypeUnknown;
}

void shimSetHandleCount(X86&, ShimCall& call) { call.result = call.u32(0); }

// ---------------------------------------------------------------------------------------
// 码页 / locale（`_setlocale` / `_setenvp` 会碰）
//    932 / 65001 走 `runtime/cp932` 的真表；其余码页只保证 ASCII（见下面的 shim）。
// ---------------------------------------------------------------------------------------
constexpr std::uint32_t kAcp = 932;  // 日语 Shift-JIS（README §2.6 的 GetACP(=932)）
// CP_ACP(0) / CP_OEMCP(1) / CP_THREAD_ACP(3) 都落到本机的 ACP（本作是 932）
bool isCp932Family(std::uint32_t cp) { return cp == 0 || cp == 1 || cp == 3 || cp == kAcp; }

void shimGetACP(X86&, ShimCall& call) { call.result = kAcp; }
void shimGetOEMCP(X86&, ShimCall& call) { call.result = kAcp; }
void shimIsValidCodePage(X86&, ShimCall& call) {
    const std::uint32_t cp = call.u32(0);
    call.result = (cp == 932 || cp == 1252 || cp == 437 || cp == 0 || cp == 1 || cp == 65001) ? 1u : 0u;
}

void shimGetCPInfo(X86& cpu, ShimCall& call) {
    const std::uint32_t cp = call.u32(0);
    const std::uint32_t info = call.u32(1);
    if (!info) {
        call.result = 0;
        return;
    }
    // CPINFO: MaxCharSize(4) + DefaultChar[2] + LeadByte[12] = 18 字节
    std::uint8_t buf[18] = {};
    const bool is932 = isCp932Family(cp);
    buf[0] = is932 ? 2 : 1;  // MaxCharSize
    if (is932) {
        buf[4] = 0x81;  // LeadByte[0] = 0x81
        buf[5] = 0x9F;
        buf[6] = 0xE0;
        buf[7] = 0xFC;
    }
    cpu.mem->writeBytes(info, buf, sizeof(buf));
    call.result = 1;
}

// 码页真表在 `runtime/cp932`（S6 码页族的落点）：932 / 65001 走真表，其余码页第一次用时记一行
// 日志并退回"字节值直通"（ASCII 正确）。⛔ 不再把 932 的每个字节糊成同一个 16 位值 —— 那会让
// 非 ASCII 字符串悄悄变成 U+00XX，而 A/W 之间往返恰好还能对齐，缺陷只能等到比较 / 排版时才炸。
constexpr std::uint32_t kCpUtf8 = 65001;
// ★ `kErrInvalidParameter(87)` 已在文件顶部（标准句柄那一节）定义 —— 全文件一份。
constexpr std::uint32_t kErrInsufficientBuffer = 122;
constexpr std::uint32_t kErrNoUnicodeTranslation = 1113;
constexpr std::uint32_t kErrInvalidFlags = 1008;  // ERROR_INVALID_FLAGS（LCMapStringW 用）
constexpr std::uint32_t kMbErrInvalidChars = 0x00000008;
constexpr std::uint32_t kWcErrInvalidChars = 0x00000080;
constexpr char16_t kReplacement = 0xFFFD;  // MultiByteToWideChar 的非法序列替换字符

void warnCodePageOnce(const char* api, std::uint32_t cp) {
    static std::string seen;
    const std::string key = std::string(api) + ":" + std::to_string(cp);
    if (seen.find(key) != std::string::npos) return;
    seen += key + ";";
    std::fprintf(stderr,
                 "[shim] kernel32!%s: 码页 %u 没有真表（只有 CP932 / 65001）；ASCII 之外按字节值直通\n",
                 api, cp);
}

std::string readGuestBytes(GuestMemory& mem, std::uint32_t ea, int len) {
    std::size_t n = 0;
    if (len < 0) {
        while (mem.rd8(ea + static_cast<std::uint32_t>(n)) != 0) ++n;
        ++n;  // 含结尾 0
    } else {
        n = static_cast<std::size_t>(len);
    }
    std::string s(n, '\0');
    for (std::size_t i = 0; i < n; ++i) s[i] = static_cast<char>(mem.rd8(ea + static_cast<std::uint32_t>(i)));
    return s;
}

std::u16string readGuestWide(GuestMemory& mem, std::uint32_t ea, int len) {
    std::u16string s;
    std::size_t n = 0;
    if (len < 0) {
        for (;;) {
            const char16_t c = static_cast<char16_t>(mem.rd16(ea + static_cast<std::uint32_t>(n) * 2u));
            s.push_back(c);
            ++n;
            if (c == 0) break;
        }
    } else {
        n = static_cast<std::size_t>(len);
        s.resize(n);
        for (std::size_t i = 0; i < n; ++i)
            s[i] = static_cast<char16_t>(mem.rd16(ea + static_cast<std::uint32_t>(i) * 2u));
    }
    return s;
}

void shimMultiByteToWideChar(X86& cpu, ShimCall& call) {
    const std::uint32_t cp = call.u32(0);
    const std::uint32_t flags = call.u32(1);
    const std::uint32_t src_ea = call.u32(2);
    const int src_len = call.i32(3);
    const std::uint32_t dst_ea = call.u32(4);
    const int dst_len = call.i32(5);
    // Win32：cbMultiByte == 0 或源指针为空 ⇒ 失败（不是"转换出 0 个字符"）
    if (src_len == 0 || src_ea == 0) {
        call.cpu->shim.last_error = kErrInvalidParameter;
        call.result = 0;
        return;
    }
    const std::string bytes = readGuestBytes(*cpu.mem, src_ea, src_len);
    std::u16string wide;
    wide.reserve(bytes.size());
    if (isCp932Family(cp)) {
        const auto* p = reinterpret_cast<const std::uint8_t*>(bytes.data());
        std::size_t i = 0;
        while (i < bytes.size()) {
            char16_t u = 0;
            const int n = cp932Decode(p + i, bytes.size() - i, &u);
            if (n == 0) {
                if ((flags & kMbErrInvalidChars) != 0) {
                    call.cpu->shim.last_error = kErrNoUnicodeTranslation;
                    call.result = 0;
                    return;
                }
                // 与 Windows 一致：非法字节 ⇒ U+FFFD，并只前进 1 字节
                wide.push_back(kReplacement);
                ++i;
                continue;
            }
            wide.push_back(u);
            i += static_cast<std::size_t>(n);
        }
    } else if (cp == kCpUtf8) {
        std::size_t bad = 0;
        wide = utf8ToUtf16(bytes, &bad, kReplacement);
        if (bad != 0 && (flags & kMbErrInvalidChars) != 0) {
            call.cpu->shim.last_error = kErrNoUnicodeTranslation;
            call.result = 0;
            return;
        }
    } else {
        warnCodePageOnce("MultiByteToWideChar", cp);
        for (const char c : bytes) wide.push_back(static_cast<char16_t>(static_cast<unsigned char>(c)));
    }
    // ★ 返回值 / 缓冲大小都是**字符数**，不是字节数（旧实现直接拿字节数糊过去）
    const std::size_t chars = wide.size();
    if (dst_ea == 0 || dst_len == 0) {
        call.result = static_cast<std::uint32_t>(chars);
        return;
    }
    if (dst_len < 0 || static_cast<std::size_t>(dst_len) < chars) {
        call.cpu->shim.last_error = kErrInsufficientBuffer;
        call.result = 0;
        return;
    }
    for (std::size_t i = 0; i < chars; ++i)
        cpu.mem->wr16(dst_ea + static_cast<std::uint32_t>(i) * 2u, wide[i]);
    call.result = static_cast<std::uint32_t>(chars);
}

void shimWideCharToMultiByte(X86& cpu, ShimCall& call) {
    const std::uint32_t cp = call.u32(0);
    const std::uint32_t flags = call.u32(1);
    const std::uint32_t src_ea = call.u32(2);
    const int src_len = call.i32(3);
    const std::uint32_t dst_ea = call.u32(4);
    const int dst_len = call.i32(5);
    const std::uint32_t default_char_ea = call.u32(6);
    const std::uint32_t used_default_ea = call.u32(7);
    if (src_len == 0 || src_ea == 0) {
        call.cpu->shim.last_error = kErrInvalidParameter;
        call.result = 0;
        return;
    }
    std::uint8_t default_byte = static_cast<std::uint8_t>('?');
    if (default_char_ea != 0) default_byte = cpu.mem->rd8(default_char_ea);
    const std::u16string text = readGuestWide(*cpu.mem, src_ea, src_len);
    std::string out;
    out.reserve(text.size() * 2u);
    bool used_default = false;
    if (isCp932Family(cp)) {
        for (std::size_t i = 0; i < text.size(); ++i) {
            const char16_t w = text[i];
            std::uint8_t b[2] = {};
            int n = 0;
            if (w >= 0xD800 && w <= 0xDFFF) {
                // 代理对（CP932 没有增补面字符）/ 孤立代理：都是一个替换字符
                const bool pair = w <= 0xDBFF && i + 1 < text.size() && text[i + 1] >= 0xDC00 &&
                                  text[i + 1] <= 0xDFFF;
                if ((flags & kWcErrInvalidChars) != 0) {
                    call.cpu->shim.last_error = kErrNoUnicodeTranslation;
                    call.result = 0;
                    return;
                }
                if (pair) ++i;
                n = 0;
            } else {
                n = cp932Encode(w, b);
            }
            if (n == 0) {
                used_default = true;
                out.push_back(static_cast<char>(default_byte));
            } else {
                out.append(reinterpret_cast<const char*>(b), static_cast<std::size_t>(n));
            }
        }
    } else if (cp == kCpUtf8) {
        std::size_t bad = 0;
        out = utf16ToUtf8(text, &bad, kReplacement);
        if (bad != 0) {
            if ((flags & kWcErrInvalidChars) != 0) {
                call.cpu->shim.last_error = kErrNoUnicodeTranslation;
                call.result = 0;
                return;
            }
            used_default = true;  // 替换字符已由 utf16ToUtf8 写进 out
        }
    } else {
        warnCodePageOnce("WideCharToMultiByte", cp);
        for (const char16_t w : text) {
            if (w > 0xFF) {
                used_default = true;
                out.push_back(static_cast<char>(default_byte));
            } else {
                out.push_back(static_cast<char>(w & 0xFF));
            }
        }
    }
    const std::size_t bytes = out.size();
    if (dst_ea == 0 || dst_len == 0) {
        call.result = static_cast<std::uint32_t>(bytes);
        return;
    }
    if (dst_len < 0 || static_cast<std::size_t>(dst_len) < bytes) {
        call.cpu->shim.last_error = kErrInsufficientBuffer;
        call.result = 0;
        return;
    }
    for (std::size_t i = 0; i < bytes; ++i)
        cpu.mem->wr8(dst_ea + static_cast<std::uint32_t>(i), static_cast<std::uint8_t>(out[i]));
    if (used_default_ea != 0) cpu.mem->wr32(used_default_ea, used_default ? 1u : 0u);
    call.result = static_cast<std::uint32_t>(bytes);
}

// ---------------------------------------------------------------------------------------
// 字符分类（`GetStringTypeW`，CRT 的 `iswalpha` / `iswspace` / `iswdigit` 一路会碰）
//
// ★ 2026-11-04 收口 REQ-01M6K69JNGE7EP8ZNRDAXNSYZZ：旧实现的**位值抄错**（'A' 被标成
//   DIGIT|ALPHA、空格被标成 CNTRL|SPACE、NUL 被标成 C1_UPPER），而且 `dwInfoType` 被
//   `(void)type` 丢弃 ⇒ 任何 info type 都按 CT_CTYPE1 **假装成功**。现在：
//     · 位值按 MSDN 的 C1_* 表（见下）；
//     · `dwInfoType != CT_CTYPE1` ⇒ 0 + ERROR_INVALID_PARAMETER(87) + 一行日志（⛔ 不静默）。
// ★ 2026-11-09 收口 REQ-01M6KH7EPPNWJDHPDQ6REV2RRM（单 1）：非 ASCII 的 CT_CTYPE1 用
//   **文档化的区间规则**（不生成表、不新增依赖；ASCII 仍走 `ctype1Ascii()` 逐位不变）：
//     · U+3000                        ⇒ SPACE|BLANK；
//     · U+3001–U+303F（日文标点）      ⇒ PUNCT（整段按标点；U+303F 是区间上界）；
//     · U+3040–U+309F（平假名）· U+30A0–U+30FF（片假名）· U+4E00–U+9FFF（CJK）⇒ ALPHA
//       （这些是 Lo 类字母；按本单口径**不**区分 UPPER/LOWER）；
//     · U+FF01–U+FF5E（全角 ASCII 变体）⇒ 按 `w - 0xFEE0` 映回 ASCII 后套 `ctype1Ascii()`
//       （全角 Ａ 与 ASCII 'A' 同一位、全角 ０ 得 DIGIT|XDIGIT、全角 ！ 得 PUNCT）；
//     · U+FF61–U+FF65（半角 ｡｢｣､･，Unicode 类别 Po）⇒ PUNCT；
//       U+FF66–U+FF9F（半角假名字母 / 浊音点）⇒ ALPHA。
//   区间外码位 ⇒ 该字符 0 位 + 一次日志（显式缺口；⛔ 不拿属性现算冒充 Windows 分类）。
//   `dwInfoType != CT_CTYPE1`（CT_CTYPE2/3）仍是 0 + 87 + 日志：没有可复现 / 可再校验的
//   Bidi_Class / C3 来源，且引擎 / CRT 不消费 ⇒ **明确不做**（合同见 contracts-win32.md）。
// ---------------------------------------------------------------------------------------
// CT_CTYPE1 位（MSDN「GetStringTypeW」的 C1_* 表）
constexpr std::uint16_t kC1Upper = 0x0001;
constexpr std::uint16_t kC1Lower = 0x0002;
constexpr std::uint16_t kC1Digit = 0x0004;
constexpr std::uint16_t kC1Space = 0x0008;
constexpr std::uint16_t kC1Punct = 0x0010;
constexpr std::uint16_t kC1Cntrl = 0x0020;
constexpr std::uint16_t kC1Blank = 0x0040;
constexpr std::uint16_t kC1Xdigit = 0x0080;
constexpr std::uint16_t kC1Alpha = 0x0100;

constexpr std::uint32_t kCtCtype1 = 1;  // CT_CTYPE1
constexpr std::uint32_t kCtCtype2 = 2;  // CT_CTYPE2（明确不做：见上）
constexpr std::uint32_t kCtCtype3 = 4;  // CT_CTYPE3（明确不做：见上）

// ASCII 的 CT_CTYPE1 位（0x00–0x7F）。文档给的定点：'A'=0x0181、'a'=0x0182、'G'=0x0101、
// '0'=0x0084、' '=0x0048、'!'=0x0010。★ 本函数是 ASCII 的唯一真源；全角 ASCII 变体也回落到它。
std::uint16_t ctype1Ascii(std::uint16_t w) {
    if (w < 0x20 || w == 0x7F) {
        // 控制字符（含 NUL / CR / LF / DEL）。TAB 同时是 blank（C99 / Unicode 的 blank = 空格 + TAB）
        return static_cast<std::uint16_t>(kC1Cntrl | (w == 0x09 ? kC1Blank : 0));
    }
    if (w == 0x20) return static_cast<std::uint16_t>(kC1Space | kC1Blank);
    if (w >= '0' && w <= '9') return static_cast<std::uint16_t>(kC1Digit | kC1Xdigit);
    if (w >= 'A' && w <= 'F') return static_cast<std::uint16_t>(kC1Upper | kC1Alpha | kC1Xdigit);
    if (w >= 'G' && w <= 'Z') return static_cast<std::uint16_t>(kC1Upper | kC1Alpha);
    if (w >= 'a' && w <= 'f') return static_cast<std::uint16_t>(kC1Lower | kC1Alpha | kC1Xdigit);
    if (w >= 'g' && w <= 'z') return static_cast<std::uint16_t>(kC1Lower | kC1Alpha);
    return kC1Punct;  // 0x21–0x2F / 0x3A–0x40 / 0x5B–0x60 / 0x7B–0x7E
}

// 非 ASCII 的 CT_CTYPE1：只做上面点名的区间；返回 false ⇒ 该字符不在覆盖范围内（0 位 + 日志）。
bool ctype1NonAscii(std::uint16_t w, std::uint16_t* bits) {
    if (w == 0x3000) {  // 全角空格
        *bits = static_cast<std::uint16_t>(kC1Space | kC1Blank);
        return true;
    }
    if (w >= 0x3001 && w <= 0x303F) {  // 日文标点（U+303F 是区间上界）
        *bits = kC1Punct;
        return true;
    }
    if ((w >= 0x3040 && w <= 0x309F) || (w >= 0x30A0 && w <= 0x30FF) ||
        (w >= 0x4E00 && w <= 0x9FFF)) {  // 平假名 / 片假名 / CJK：Lo 类字母
        *bits = kC1Alpha;
        return true;
    }
    if (w >= 0xFF01 && w <= 0xFF5E) {  // 全角 ASCII 变体：映回 ASCII 后套 ASCII 的位
        *bits = ctype1Ascii(static_cast<std::uint16_t>(w - 0xFEE0));
        return true;
    }
    if (w >= 0xFF61 && w <= 0xFF9F) {  // 半角片假名：U+FF61–FF65 是 Po 标点，其余是假名字母
        *bits = (w <= 0xFF65) ? kC1Punct : kC1Alpha;
        return true;
    }
    return false;
}

void shimGetStringTypeW(X86& cpu, ShimCall& call) {
    const std::uint32_t type = call.u32(0);
    const std::uint32_t src = call.u32(1);
    const std::int32_t n = call.i32(2);
    const std::uint32_t dst = call.u32(3);
    if (type != kCtCtype1) {
        // CT_CTYPE2 / CT_CTYPE3 在真机上是**合法**值（这里没实现）⇒ 明确失败而不是按 CT_CTYPE1 假装成功
        cpu.shim.last_error = kErrInvalidParameter;
        shimLog("kernel32", "GetStringTypeW",
                "dwInfoType=%u 未实现（本模拟器只做 CT_CTYPE1=%u / 不做 CT_CTYPE2=%u、CT_CTYPE3=%u）"
                " ⇒ 返回 0（ERROR_INVALID_PARAMETER）",
                type, kCtCtype1, kCtCtype2, kCtCtype3);
        call.result = 0;
        return;
    }
    if (!src || !dst || n == 0 || n < -1) {
        cpu.shim.last_error = kErrInvalidParameter;
        shimLog("kernel32", "GetStringTypeW", "参数非法（lpSrcStr=0x%08X lpCharType=0x%08X cchSrc=%d）"
                                               " ⇒ 返回 0（ERROR_INVALID_PARAMETER）",
                src, dst, n);
        call.result = 0;
        return;
    }
    // cchSrc = -1 ⇒ NUL 结尾（结尾的那个 0 **不**参与分类，与文档一致）
    std::uint32_t count = 0;
    if (n < 0) {
        while (cpu.mem->rd16(src + count * 2u) != 0) ++count;
    } else {
        count = static_cast<std::uint32_t>(n);
    }
    std::uint32_t unclassified = 0;
    std::uint16_t first_unclassified = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint16_t w = cpu.mem->rd16(src + i * 2u);
        std::uint16_t flags = 0;
        bool classified = true;
        if (w < 0x80) {
            flags = ctype1Ascii(w);
        } else {
            classified = ctype1NonAscii(w, &flags);  // ⛔ 区间外不猜：0 位 + 下面的日志
        }
        if (!classified) {
            if (unclassified == 0) first_unclassified = w;
            ++unclassified;
        }
        cpu.mem->wr16(dst + i * 2u, flags);
    }
    if (unclassified != 0) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            shimLog("kernel32", "GetStringTypeW",
                    "CT_CTYPE1 的区间规则没有覆盖 U+%04X（本次 %u 个字符按 0 位返回）",
                    static_cast<unsigned>(first_unclassified), unclassified);
        }
    }
    call.result = 1;
}

// ---------------------------------------------------------------------------------------
// 大小写映射（`LCMapStringW`，CRT 的 `_wcsicmp` / `towlower` 一路会碰）
//
// ★ 2026-11-04 收口 REQ-01M6K69K28AT5VNKZAJXT6PZJ4：旧实现 `cchSrc=-1` 只映射 **1** 个字符
//   （返回 1）⇒ 调用方按返回值认为"整串已转换"，静默截断。现在：
//     · `cchSrc=-1` 扫到 NUL，**含**结尾 0 计数（文档：cchSrc<0 时返回值含结尾 0）；
//     · `cchDest==0` 是"问大小"（返回同一数值）；缓冲不足 ⇒ 0 + 122；cchDest<0 / lpDestStr=0
//       而 cchDest>0 ⇒ 0 + 87；
//     · 未实现的 `dwMapFlags`（SORTKEY / LINGUISTIC_CASING …）⇒ 0 + ERROR_INVALID_FLAGS(1008)
//       （⛔ 旧实现静默按"无映射"成功）。
// ★ 2026-11-09 收口 REQ-01M6KH7HC3ZP3M5RS8EJ3813WC（单 2）：
//   · HIRAGANA / KATAKANA：假名 1:1 区间（U+3041–U+3096 ⇄ U+30A1–U+30F6，±0x60；
//     另外 ゝゞゟ ⇄ ヽヾヿ，U+309D–U+309F ⇄ U+30FD–U+30FF，同一偏移）。
//     ー(U+30FC) / ゛゜(U+3099–309C、U+FF9E/FF9F) 是平假名/片假名**共享**的记号 ⇒ 不动；
//     ヷヸヹヺ(U+30F7–30FA) 没有预组合平假名 ⇒ 不动；ゔ/ゕ/ゖ(U+3094–3096) 在偏移区间内，自测见 CTest。
//   · HALFWIDTH / FULLWIDTH：ASCII 部分纯算术（0x20 ⇄ U+3000、0x21–0x7E ⇄ U+FF01–FF5E，±0xFEE0）；
//     半角片假名 U+FF61–U+FF9F ⇄ 全角用下面那张 **63 项 Unicode 标准 1:1 映射**
//     （来源：Unicode「Halfwidth and Fullwidth Forms」的兼容映射；逐项列在 kKanaWidth）。
//     不做 NFC 合成（文档只说 "may"；「は + U+3099」不合成「ば」）。
//   · 宽度先于假名：文档明说 HIRAGANA 只有配上 FULLWIDTH 才能把**半角**片假名映到平假名；
//   · BYTEREV 逐 16 位码元反转字节（MSDN 例：0x3450 0x4822 ⇒ 0x5034 0x2248）；
//   · 文档点名的互斥对（LOWERCASE|UPPERCASE / HIRAGANA|KATAKANA / HALFWIDTH|FULLWIDTH）
//     ⇒ 0 + 1008；
//   · SORTKEY 仍**明确不做**：没有可复现 / 可再校验的 NLS 排序权重来源 ⇒ 0 + 1008 + 日志（合同登记）。
// ---------------------------------------------------------------------------------------
constexpr std::uint32_t kLcmapLowercase = 0x00000100;
constexpr std::uint32_t kLcmapUppercase = 0x00000200;
constexpr std::uint32_t kLcmapSortkey = 0x00000400;  // 明确不做：见上
constexpr std::uint32_t kLcmapByterev = 0x00000800;
constexpr std::uint32_t kLcmapHiragana = 0x00100000;
constexpr std::uint32_t kLcmapKatakana = 0x00200000;
constexpr std::uint32_t kLcmapHalfwidth = 0x00400000;
constexpr std::uint32_t kLcmapFullwidth = 0x00800000;
static_assert(kLcmapSortkey == 0x00000400, "LCMAP_SORTKEY 的 Win32 数值");
constexpr std::uint32_t kLcmapCaseMask = kLcmapLowercase | kLcmapUppercase;
constexpr std::uint32_t kLcmapKanaMask = kLcmapHiragana | kLcmapKatakana;
constexpr std::uint32_t kLcmapWidthMask = kLcmapHalfwidth | kLcmapFullwidth;
constexpr std::uint32_t kLcmapDoable =
    kLcmapCaseMask | kLcmapByterev | kLcmapKanaMask | kLcmapWidthMask;

// ---- 假名 / 全半角映射（§2026-11-09 单 2；全部是文档化区间 / 标准 1:1 映射，⛔ 不引入依赖） ----

// 假名区间：U+3041–U+3096 ⇄ U+30A1–U+30F6（±0x60）；ゝゞゟ(U+309D–U+309F) ⇄ ヽヾヿ(U+30FD–U+30FF)。
// ー(U+30FC) 与组合/独立浊音点（U+3099–U+309C）是**共享**记号 ⇒ 原样；ヷヸヹヺ(U+30F7–30FA) 无平假名形式。
constexpr std::uint16_t kKanaOffset = 0x60;

std::uint16_t mapKatakanaToHiragana(std::uint16_t w) {
    if (w >= 0x30A1 && w <= 0x30F6) return static_cast<std::uint16_t>(w - kKanaOffset);
    if (w >= 0x30FD && w <= 0x30FF) return static_cast<std::uint16_t>(w - kKanaOffset);
    return w;
}

std::uint16_t mapHiraganaToKatakana(std::uint16_t w) {
    if (w >= 0x3041 && w <= 0x3096) return static_cast<std::uint16_t>(w + kKanaOffset);
    if (w >= 0x309D && w <= 0x309F) return static_cast<std::uint16_t>(w + kKanaOffset);
    return w;
}

// 半角片假名 → 全角：Unicode「Halfwidth and Fullwidth Forms」的 63 项 1:1 兼容映射
// （U+FF61–U+FF9F 全部在内；U+FF9E/FF9F 映到组合浊音点 U+3099/309A）。
struct KanaWidthPair {
    std::uint16_t half;
    std::uint16_t full;
};

constexpr KanaWidthPair kKanaWidth[] = {
    {0xFF61, 0x3002},  // ｡ → 。
    {0xFF62, 0x300C},  // ｢ → 「
    {0xFF63, 0x300D},  // ｣ → 」
    {0xFF64, 0x3001},  // ､ → 、
    {0xFF65, 0x30FB},  // ･ → ・
    {0xFF66, 0x30F2},  // ｦ → ヲ
    {0xFF67, 0x30A1},  // ｧ → ァ
    {0xFF68, 0x30A3},  // ｨ → ィ
    {0xFF69, 0x30A5},  // ｩ → ゥ
    {0xFF6A, 0x30A7},  // ｪ → ェ
    {0xFF6B, 0x30A9},  // ｫ → ォ
    {0xFF6C, 0x30E3},  // ｬ → ャ
    {0xFF6D, 0x30E5},  // ｭ → ュ
    {0xFF6E, 0x30E7},  // ｮ → ョ
    {0xFF6F, 0x30C3},  // ｯ → ッ
    {0xFF70, 0x30FC},  // ｰ → ー
    {0xFF71, 0x30A2},  // ｱ → ア
    {0xFF72, 0x30A4},  // ｲ → イ
    {0xFF73, 0x30A6},  // ｳ → ウ
    {0xFF74, 0x30A8},  // ｴ → エ
    {0xFF75, 0x30AA},  // ｵ → オ
    {0xFF76, 0x30AB},  // ｶ → カ
    {0xFF77, 0x30AD},  // ｷ → キ
    {0xFF78, 0x30AF},  // ｸ → ク
    {0xFF79, 0x30B1},  // ｹ → ケ
    {0xFF7A, 0x30B3},  // ｺ → コ
    {0xFF7B, 0x30B5},  // ｻ → サ
    {0xFF7C, 0x30B7},  // ｼ → シ
    {0xFF7D, 0x30B9},  // ｽ → ス
    {0xFF7E, 0x30BB},  // ｾ → セ
    {0xFF7F, 0x30BD},  // ｿ → ソ
    {0xFF80, 0x30BF},  // ﾀ → タ
    {0xFF81, 0x30C1},  // ﾁ → チ
    {0xFF82, 0x30C4},  // ﾂ → ツ
    {0xFF83, 0x30C6},  // ﾃ → テ
    {0xFF84, 0x30C8},  // ﾄ → ト
    {0xFF85, 0x30CA},  // ﾅ → ナ
    {0xFF86, 0x30CB},  // ﾆ → ニ
    {0xFF87, 0x30CC},  // ﾇ → ヌ
    {0xFF88, 0x30CD},  // ﾈ → ネ
    {0xFF89, 0x30CE},  // ﾉ → ノ
    {0xFF8A, 0x30CF},  // ﾊ → ハ
    {0xFF8B, 0x30D2},  // ﾋ → ヒ
    {0xFF8C, 0x30D5},  // ﾌ → フ
    {0xFF8D, 0x30D8},  // ﾍ → ヘ
    {0xFF8E, 0x30DB},  // ﾎ → ホ
    {0xFF8F, 0x30DE},  // ﾏ → マ
    {0xFF90, 0x30DF},  // ﾐ → ミ
    {0xFF91, 0x30E0},  // ﾑ → ム
    {0xFF92, 0x30E1},  // ﾒ → メ
    {0xFF93, 0x30E2},  // ﾓ → モ
    {0xFF94, 0x30E4},  // ﾔ → ヤ
    {0xFF95, 0x30E6},  // ﾕ → ユ
    {0xFF96, 0x30E8},  // ﾖ → ヨ
    {0xFF97, 0x30E9},  // ﾗ → ラ
    {0xFF98, 0x30EA},  // ﾘ → リ
    {0xFF99, 0x30EB},  // ﾙ → ル
    {0xFF9A, 0x30EC},  // ﾚ → レ
    {0xFF9B, 0x30ED},  // ﾛ → ロ
    {0xFF9C, 0x30EF},  // ﾜ → ワ
    {0xFF9D, 0x30F3},  // ﾝ → ン
    {0xFF9E, 0x3099},  // ﾞ → ゛（组合浊音点）
    {0xFF9F, 0x309A},  // ﾟ → ゜（组合半浊音点）
};
static_assert(sizeof(kKanaWidth) / sizeof(kKanaWidth[0]) == 63, "半角片假名 1:1 映射必须是 63 项");

bool kanaWidthToFull(std::uint16_t w, std::uint16_t* full) {
    for (const KanaWidthPair& p : kKanaWidth) {
        if (p.half == w) {
            *full = p.full;
            return true;
        }
    }
    return false;
}

bool kanaWidthToHalf(std::uint16_t w, std::uint16_t* half) {
    for (const KanaWidthPair& p : kKanaWidth) {
        if (p.full == w) {
            *half = p.half;
            return true;
        }
    }
    return false;
}

// FULLWIDTH：ASCII 空格 → U+3000；0x21–0x7E → ＋0xFEE0；半角片假名 → 上面的标准全角。
std::uint16_t mapFullwidth(std::uint16_t w) {
    if (w == 0x20) return 0x3000;
    if (w >= 0x21 && w <= 0x7E) return static_cast<std::uint16_t>(w + 0xFEE0);
    std::uint16_t full = 0;
    return kanaWidthToFull(w, &full) ? full : w;
}

// HALFWIDTH：U+3000 → ASCII 空格；U+FF01–FF5E → －0xFEE0；标准全角的片假名 → 半角。
std::uint16_t mapHalfwidth(std::uint16_t w) {
    if (w == 0x3000) return 0x20;
    if (w >= 0xFF01 && w <= 0xFF5E) return static_cast<std::uint16_t>(w - 0xFEE0);
    std::uint16_t half = 0;
    return kanaWidthToHalf(w, &half) ? half : w;
}

void shimLCMapStringW(X86& cpu, ShimCall& call) {
    // lcid：本模拟器只有 en-US 一个 locale（GetUserDefaultLCID），不参与映射
    const std::uint32_t lcid = call.u32(0);
    (void)lcid;
    const std::uint32_t flags = call.u32(1);
    const std::uint32_t src = call.u32(2);
    const std::int32_t n = call.i32(3);
    const std::uint32_t dst = call.u32(4);
    const std::int32_t cap = call.i32(5);

    if (!src || n == 0 || n < -1) {
        cpu.shim.last_error = kErrInvalidParameter;
        shimLog("kernel32", "LCMapStringW", "参数非法（lpSrcStr=0x%08X cchSrc=%d）⇒ 返回 0"
                                               "（ERROR_INVALID_PARAMETER）",
                src, n);
        call.result = 0;
        return;
    }
    const std::uint32_t unsupported = flags & ~kLcmapDoable;
    if (unsupported != 0 || (flags & kLcmapCaseMask) == kLcmapCaseMask ||
        (flags & kLcmapKanaMask) == kLcmapKanaMask ||
        (flags & kLcmapWidthMask) == kLcmapWidthMask) {
        cpu.shim.last_error = kErrInvalidFlags;  // ERROR_INVALID_FLAGS
        shimLog("kernel32", "LCMapStringW",
                "dwMapFlags=0x%08X 里有未实现的位（0x%08X）或互斥组合"
                "（LOWERCASE|UPPERCASE / HIRAGANA|KATAKANA / HALFWIDTH|FULLWIDTH）"
                " ⇒ 返回 0（ERROR_INVALID_FLAGS）",
                flags, unsupported);
        call.result = 0;
        return;
    }
    // 要映射的字符数：cchSrc<0 ⇒ 扫到 NUL，**含**结尾 0（文档：cchSrc=-1 时返回值含结尾 0）
    std::uint32_t count = 0;
    if (n < 0) {
        while (cpu.mem->rd16(src + count * 2u) != 0) ++count;
        ++count;
    } else {
        count = static_cast<std::uint32_t>(n);
    }
    if (cap < 0) {
        cpu.shim.last_error = kErrInvalidParameter;
        shimLog("kernel32", "LCMapStringW", "cchDest=%d < 0 ⇒ 返回 0（ERROR_INVALID_PARAMETER）", cap);
        call.result = 0;
        return;
    }
    if (cap == 0) {  // 文档：问大小 —— 返回所需的字符数（cchSrc<0 时含结尾 0），不碰 lpDestStr
        call.result = count;
        return;
    }
    if (!dst) {
        cpu.shim.last_error = kErrInvalidParameter;
        shimLog("kernel32", "LCMapStringW", "cchDest=%d 但 lpDestStr 为空 ⇒ 返回 0"
                                               "（ERROR_INVALID_PARAMETER）",
                cap);
        call.result = 0;
        return;
    }
    if (static_cast<std::uint32_t>(cap) < count) {
        cpu.shim.last_error = kErrInsufficientBuffer;
        shimLog("kernel32", "LCMapStringW", "缓冲不足（需要 %u 个字符，cchDest=%d）⇒ 返回 0"
                                               "（ERROR_INSUFFICIENT_BUFFER）",
                count, cap);
        call.result = 0;
        return;
    }
    const bool lower = (flags & kLcmapLowercase) != 0;
    const bool upper = (flags & kLcmapUppercase) != 0;
    const bool byterev = (flags & kLcmapByterev) != 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint16_t w = cpu.mem->rd16(src + i * 2u);
        if (lower && w >= 'A' && w <= 'Z') w = static_cast<std::uint16_t>(w - 'A' + 'a');
        else if (upper && w >= 'a' && w <= 'z') w = static_cast<std::uint16_t>(w - 'a' + 'A');
        // ★ 宽度先于假名：文档明说 HIRAGANA 不配 FULLWIDTH 时**半角**片假名原样留在目标串里。
        if ((flags & kLcmapHalfwidth) != 0) w = mapHalfwidth(w);
        if ((flags & kLcmapFullwidth) != 0) w = mapFullwidth(w);
        if ((flags & kLcmapHiragana) != 0) w = mapKatakanaToHiragana(w);
        if ((flags & kLcmapKatakana) != 0) w = mapHiraganaToKatakana(w);
        // BYTEREV：逐 16 位码元反转字节（不是反转字符顺序；NUL 仍映射到自身）
        if (byterev) w = static_cast<std::uint16_t>((w >> 8) | (w << 8));
        cpu.mem->wr16(dst + i * 2u, w);
    }
    // 文档：返回值 = 映射后字符串的字符数（cchSrc<0 时含结尾 0）
    call.result = count;
}

void shimIsValidLocale(X86&, ShimCall& call) { call.result = 1; }
// ★ `GetLocaleInfoA` 已搬到 `kernel32_string.cpp`（与 W 共用一张 locale 表；2026-11-04）。
void shimGetUserDefaultLCID(X86&, ShimCall& call) { call.result = 0x0409; }
void shimGetSystemDefaultLCID(X86&, ShimCall& call) { call.result = 0x0409; }
void shimGetThreadLocale(X86&, ShimCall& call) { call.result = 0x0409; }
void shimSetThreadLocale(X86&, ShimCall& call) { call.result = 1; }
void shimGetUserDefaultUILanguage(X86&, ShimCall& call) { call.result = 0x0409; }

// ---------------------------------------------------------------------------------------
// 环境块（`_setenvp` 会碰）
//
// ★ **唯一真源** = `lift::guestEnvironment(...)`（vfs.h）：与 `setupProcessStructures` 写进 guest
//   的那一块、`SHGetSpecialFolderPathA` / `SHGetFolderPathA` 返回的路径**同源**。
//   ⛔ 不许在这里再写一份 `PATH=.` / `TEMP=...`（两套文本必然漂，2026-11-05 的 bug 就是这么来的）。
// ---------------------------------------------------------------------------------------
std::vector<std::string> envItems(const X86& cpu) {
    const bool have_appdata = cpu.vfs && !cpu.vfs->appDataDir().empty();
    return lift::guestEnvironment(have_appdata);
}

// 环境块的 UTF-16 视图（`NAME=VALUE\0…\0\0`）。`ea == 0` ⇒ 从真源现造一块。
std::uint32_t envBlockWide(X86& cpu) {
    if (cpu.shim.env_ea != 0) return cpu.shim.env_ea;
    // 纯档夹具 / 没走 prepare 的调用：在 0x7FFD5000 现造（与本模拟器的 argv 块相邻的保留页）
    const std::uint32_t ea = 0x7FFD5000u;
    if (!cpu.mem->committed(ea, 0x1000)) {
        if (!cpu.mem->commitZeroed(ea, 0x1000, RegionKind::Other, "environment")) return 0;
    }
    std::uint32_t off = 0;
    for (const std::string& item : envItems(cpu)) {
        for (const char c : item) {
            cpu.mem->wr16(ea + off, static_cast<std::uint16_t>(static_cast<unsigned char>(c)));
            off += 2;
        }
        cpu.mem->wr16(ea + off, 0);
        off += 2;
    }
    cpu.mem->wr16(ea + off, 0);
    return ea;
}

// 环境块的 ANSI 视图（`NAME=VALUE\0…\0\0`）。每次调用在 guest 堆上新建一块：
// `FreeEnvironmentStringsA` 在本模拟器里是 no-op（CRT 只在启动时取一次）⇒ 会留下一个
// 进程生命期的小块；⛔ 不缓存 EA（缓存要往 X86::ShimState 加字段，那是别人的文件）。
std::uint32_t envBlockAnsi(X86& cpu) {
    if (!cpu.heap) return 0;
    std::string bytes;
    for (const std::string& item : envItems(cpu)) {
        bytes += item;
        bytes.push_back('\0');
    }
    bytes.push_back('\0');
    const std::uint32_t ea =
        cpu.heap->alloc(static_cast<std::uint32_t>(bytes.size()), /*zero=*/true);
    if (ea == 0) return 0;
    cpu.mem->writeBytes(ea, bytes.data(), bytes.size());
    return ea;
}

void shimGetEnvironmentStringsW(X86& cpu, ShimCall& call) { call.result = envBlockWide(cpu); }
void shimGetEnvironmentStringsA(X86& cpu, ShimCall& call) { call.result = envBlockAnsi(cpu); }

void shimFreeEnvironmentStringsW(X86&, ShimCall& call) { call.result = 1; }
void shimFreeEnvironmentStringsA(X86&, ShimCall& call) { call.result = 1; }

// 取值：从**同一份真源**里按名字查（大小写不敏感）。文档口径：
// 缓冲够 ⇒ 返回长度（不含结尾 0）；不够 ⇒ 返回所需字节数（**含**结尾 0）；没有 ⇒ 0 + 203。
std::uint32_t getEnvValue(X86& cpu, ShimCall& call, bool wide) {
    // ★ A 版的名字是 CP932 字节；W 版是 UTF-16 ⇒ 两种都要读对（⛔ 不能拿 A 的 `call.str(0)` 去读 W）。
    std::string name;
    if (wide) {
        for (std::uint32_t i = 0; i < 256u; ++i) {
            const std::uint16_t w = cpu.mem->rd16(call.u32(0) + i * 2u);
            if (w == 0) break;
            name.push_back(w < 0x80 ? static_cast<char>(w) : '?');
        }
    } else {
        name = call.str(0);
    }
    const std::uint32_t buf = call.u32(1);
    const std::uint32_t cap = call.u32(2);
    std::string value;
    bool found = false;
    for (const std::string& item : envItems(cpu)) {
        const std::size_t eq = item.find('=');
        if (eq == std::string::npos) continue;
        if (!equalsIgnoreCase(name, item.substr(0, eq))) continue;
        value = item.substr(eq + 1);
        found = true;
        break;
    }
    if (!found) {
        cpu.shim.last_error = 203;  // ERROR_ENVVAR_NOT_FOUND
        call.result = 0;
        return 0;
    }
    const std::size_t need = value.size() + 1u;  // 字符数（本环境全是 ASCII ⇒ 字节数相同）
    if (cap < need) {
        call.result = static_cast<std::uint32_t>(need);
        return call.result;
    }
    if (buf == 0) {
        cpu.shim.last_error = 87;  // ERROR_INVALID_PARAMETER（与其它"缓冲非 0 但指针为空"一致）
        call.result = 0;
        return 0;
    }
    if (wide) {
        for (std::size_t i = 0; i < value.size(); ++i) {
            cpu.mem->wr16(buf + static_cast<std::uint32_t>(i) * 2u,
                          static_cast<std::uint16_t>(static_cast<unsigned char>(value[i])));
        }
        cpu.mem->wr16(buf + static_cast<std::uint32_t>(value.size()) * 2u, 0);
    } else {
        guestWrite(*cpu.mem, buf, value);
    }
    call.result = static_cast<std::uint32_t>(value.size());
    return call.result;
}

void shimGetEnvironmentVariableA(X86& cpu, ShimCall& call) { getEnvValue(cpu, call, false); }
void shimGetEnvironmentVariableW(X86& cpu, ShimCall& call) { getEnvValue(cpu, call, true); }

// ---------------------------------------------------------------------------------------
// CPU 特性 / 错误模式 / 虚拟内存查询（启动早期会碰）
// ---------------------------------------------------------------------------------------
void shimIsProcessorFeaturePresent(X86&, ShimCall& call) {
    // ★ 确定性（plan §8.3-2）：不接宿主 CPU。返回"什么都没有的 586"⇒ CRT 选安全的慢路径。
    (void)call.u32(0);
    call.result = 0;
}

void shimSetErrorMode(X86&, ShimCall& call) { call.result = 0; }

void shimVirtualQuery(X86& cpu, ShimCall& call) {
    const std::uint32_t addr = call.u32(0);
    const std::uint32_t info = call.u32(1);
    const std::uint32_t len = call.u32(2);
    if (!info || len < 28) {
        call.result = 0;
        return;
    }
    // MEMORY_BASIC_INFORMATION (28 字节 on x86)
    std::uint8_t mbi[28] = {};
    const Region* r = cpu.mem->regionOf(addr);
    std::uint32_t base = r ? r->begin : (addr & ~0xFFFu);
    std::uint32_t end = r ? r->end : ((addr & ~0xFFFu) + 0x1000u);
    if (end <= addr) end = addr + 0x1000u;
    const bool committed = cpu.mem->committed(addr, 1);
    const std::uint32_t state = committed ? 0x1000u : 0x10000u;  // MEM_COMMIT / MEM_FREE
    const std::uint32_t protect = committed ? 0x04u : 0x01u;     // PAGE_READWRITE / PAGE_NOACCESS
    auto put = [&](int off, std::uint32_t v) { std::memcpy(mbi + off, &v, 4); };
    put(0, base);       // BaseAddress
    put(4, base);       // AllocationBase
    put(8, protect);    // AllocationProtect
    put(12, end - base);  // RegionSize
    put(16, state);     // State
    put(20, protect);   // Protect
    put(24, committed ? 0x20000u : 0u);  // Type（MEM_PRIVATE / 0）
    cpu.mem->writeBytes(info, mbi, sizeof(mbi));
    call.result = 28;
}

void shimGetSystemTimePreciseAsFileTime(X86& cpu, ShimCall& call) {
    const std::uint32_t ea = call.u32(0);
    const std::uint64_t ft = cpu.clock->now100ns() + 0x019DB1DED53E8000ull;  // 1601→1970 的偏移
    if (ea) cpu.mem->wr64(ea, ft);
    call.result = 0;
}

void shimSetConsoleCtrlHandler(X86&, ShimCall& call) { call.result = 1; }
// 文档（GetConsoleMode）：lpMode 是**控制台**句柄的模式；非控制台句柄 ⇒ 0 + ERROR_INVALID_HANDLE(6)。
// ★ 旧实现对所有句柄都成功（还一律填输入模式）⇒ CRT / guest 无法区分"控制台"与"磁盘文件"。
// 本模拟器没有真控制台：**只有标准槽里的三个假句柄**算控制台（保持启动路径现状）。
void shimGetConsoleMode(X86& cpu, ShimCall& call) {
    const std::uint32_t h = call.u32(0);
    const std::uint32_t out = call.u32(1);
    if (out == 0) {
        cpu.shim.last_error = kErrInvalidParameter;
        shimLog("kernel32", "GetConsoleMode", "lpMode 为空 ⇒ 返回 0（ERROR_INVALID_PARAMETER）");
        call.result = 0;
        return;
    }
    if (cpu.handles) {
        if (const HandleEntry* e = cpu.handles->get(h)) {
            cpu.shim.last_error = kErrInvalidHandle;
            shimLog("kernel32", "GetConsoleMode",
                    "0x%08X 是 %s 句柄，不是控制台 ⇒ 返回 0（ERROR_INVALID_HANDLE）", h,
                    handleKindName(e->kind));
            call.result = 0;
            return;
        }
        if (cpu.handles->stdSlotOf(h) >= 0) {
            cpu.mem->wr32(out, 0x0003u);  // ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT
            call.result = 1;
            return;
        }
    }
    cpu.shim.last_error = kErrInvalidHandle;
    shimLog("kernel32", "GetConsoleMode", "0x%08X 不是控制台句柄 ⇒ 返回 0（ERROR_INVALID_HANDLE）", h);
    call.result = 0;
}
void shimGetConsoleOutputCP(X86&, ShimCall& call) { call.result = 932; }
void shimSetConsoleOutputCP(X86&, ShimCall& call) { call.result = 1; }

// ---- ole32 的 COM 内存 / 初始化（与 kernel32 的同名 API 走同一个 guest 堆）----
void shimCoTaskMemAlloc(X86& cpu, ShimCall& call) {
    const std::uint32_t n = call.u32(0);
    const std::uint32_t p = cpu.heap->alloc(n ? n : 1, false);
    if (!p) cpu.shim.last_error = 8;  // ERROR_NOT_ENOUGH_MEMORY
    call.result = p;
}
void shimCoTaskMemFree(X86& cpu, ShimCall& call) {
    const std::uint32_t p = call.u32(0);
    if (p) cpu.heap->free(p);
    call.result = 0;
}
void shimCoInitialize(X86&, ShimCall& call) { call.result = 0; }   // S_OK（单线程 apartment）
void shimCoUninitialize(X86&, ShimCall& call) { call.result = 0; }
void shimVoidOk(X86&, ShimCall& call) { call.result = 0; }
void shimCoAddRefServerProcess(X86&, ShimCall& call) { call.result = 1; }

// ---------------------------------------------------------------------------------------
// SEH / C++ EH 的两个 OS 原语（`REQ-01M708DF24KY0QZNEHY66S0HYW`）
//
// ★ 这里**只搬参数**：真正的派发在 `Machine::pumpSeh`（块边界）——⛔ 不在 shim 里跑 guest。
//   guest 自己的 `_CxxThrowException` 用 `call dword ptr [0x51d1dc]`（0x4E8AB0）调进来，
//   所以 `call.ret_ea` 就是 `EXCEPTION_RECORD.ExceptionAddress`（Win32 文档语义）。
// ★ 四个实参**原样透传**：guest 在 `CallCatchBlock` 里自己校验
//   `code@+0x00` / `NumberParameters@+0x10` / `ExceptionInformation[0]@+0x14`
//   （0x4F00F8 / 0x4F0100 / 0x4F0109：`Info[0] ∈ {0x19930520,21,22}`）——宿主合成任何一个值都会
//   让那个 catch **静默不成立**。
// ★ 交接方式 = `take_control`：esp 铺成"形如 stdcall 返回"、eip = `kSehDispatchEa`；
//   下一次块边界由 `Machine::pumpSeh` 认领。失败（结构铺不下等）也已写进 pending stop ⇒ 响亮停。
// ---------------------------------------------------------------------------------------
void shimRaiseException(X86& cpu, ShimCall& call) {
    Machine* machine = cpu.machine;
    if (machine == nullptr) {
        // ⛔ 不许静默 no-op：没有状态机就只能响亮停（`Trap` ⇒ 非 0 退出）。
        std::fprintf(stderr, "[seh] RaiseException 无法派发：这台 cpu 没有 Machine 归属\n");
        cpu.running = false;
        cpu.stop_why = "kernel32!RaiseException：cpu 没有 Machine 归属（无法派发）";
        cpu.stop_kind = HostStopKind::Trap;
        return;
    }
    (void)machine->seh().beginRaise(cpu, *cpu.mem, call.u32(0), call.u32(1), call.u32(2), call.u32(3),
                                    cpu.esp(), call.ret_ea);
    call.take_control = true;
    cpu.esp() += 4u + call.stdcall_bytes;
    cpu.eip = kSehDispatchEa;
}

void shimRtlUnwind(X86& cpu, ShimCall& call) {
    Machine* machine = cpu.machine;
    if (machine == nullptr) {
        std::fprintf(stderr, "[seh] RtlUnwind 无法派发：这台 cpu 没有 Machine 归属\n");
        cpu.running = false;
        cpu.stop_why = "kernel32!RtlUnwind：cpu 没有 Machine 归属（无法派发）";
        cpu.stop_kind = HostStopKind::Trap;
        return;
    }
    // (TargetFrame, TargetIp, ExceptionRecord, ReturnValue)（stdcall 4 参，右到左压栈）
    (void)machine->seh().beginUnwind(cpu, *cpu.mem, call.u32(0), call.u32(1), call.u32(2), call.u32(3),
                                     cpu.esp(), call.ret_ea);
    call.take_control = true;
    cpu.esp() += 4u + call.stdcall_bytes;
    cpu.eip = kSehDispatchEa;
}

}  // namespace

// 由 machine.cpp 在 registerKernel32Shims() 之后调用
void registerBootstrapShims(ShimRegistry& s) {
    s.add("kernel32", "HeapSetInformation", Conv::Stdcall, 4, &shimHeapSetInformation);
    s.add("kernel32", "HeapQueryInformation", Conv::Stdcall, 5, &shimHeapQueryInformation);
    s.add("kernel32", "HeapValidate", Conv::Stdcall, 3, &shimHeapValidate);
    s.add("kernel32", "HeapCompact", Conv::Stdcall, 2, &shimHeapCompact);
    s.add("kernel32", "HeapLock", Conv::Stdcall, 1, &shimHeapLock);
    s.add("kernel32", "HeapUnlock", Conv::Stdcall, 1, &shimHeapUnlock);
    s.add("kernel32", "GetProcessHeaps", Conv::Stdcall, 2, &shimGetProcessHeaps);

    s.add("kernel32", "GetStdHandle", Conv::Stdcall, 1, &shimGetStdHandle);
    s.add("kernel32", "SetStdHandle", Conv::Stdcall, 2, &shimSetStdHandle);
    s.add("kernel32", "GetFileType", Conv::Stdcall, 1, &shimGetFileType);
    s.add("kernel32", "SetHandleCount", Conv::Stdcall, 1, &shimSetHandleCount);

    s.add("kernel32", "GetACP", Conv::Stdcall, 0, &shimGetACP);
    s.add("kernel32", "GetOEMCP", Conv::Stdcall, 0, &shimGetOEMCP);
    s.add("kernel32", "IsValidCodePage", Conv::Stdcall, 1, &shimIsValidCodePage);
    s.add("kernel32", "GetCPInfo", Conv::Stdcall, 2, &shimGetCPInfo);
    s.add("kernel32", "MultiByteToWideChar", Conv::Stdcall, 6, &shimMultiByteToWideChar);
    s.add("kernel32", "WideCharToMultiByte", Conv::Stdcall, 8, &shimWideCharToMultiByte);
    s.add("kernel32", "GetStringTypeW", Conv::Stdcall, 4, &shimGetStringTypeW);
    s.add("kernel32", "LCMapStringW", Conv::Stdcall, 6, &shimLCMapStringW);
    s.add("kernel32", "IsValidLocale", Conv::Stdcall, 2, &shimIsValidLocale);
    s.add("kernel32", "GetUserDefaultLCID", Conv::Stdcall, 0, &shimGetUserDefaultLCID);
    s.add("kernel32", "GetSystemDefaultLCID", Conv::Stdcall, 0, &shimGetSystemDefaultLCID);
    s.add("kernel32", "GetThreadLocale", Conv::Stdcall, 0, &shimGetThreadLocale);
    s.add("kernel32", "SetThreadLocale", Conv::Stdcall, 1, &shimSetThreadLocale);
    s.add("kernel32", "GetUserDefaultUILanguage", Conv::Stdcall, 0, &shimGetUserDefaultUILanguage);

    // 环境族：**同一份真源**（vfs.h 的 guestEnvironment）；A/W 两版都实现（⛔ 不各写一份字符串）
    s.add("kernel32", "GetEnvironmentStringsW", Conv::Stdcall, 0, &shimGetEnvironmentStringsW);
    s.add("kernel32", "GetEnvironmentStringsA", Conv::Stdcall, 0, &shimGetEnvironmentStringsA);
    s.add("kernel32", "FreeEnvironmentStringsW", Conv::Stdcall, 1, &shimFreeEnvironmentStringsW);
    s.add("kernel32", "FreeEnvironmentStringsA", Conv::Stdcall, 1, &shimFreeEnvironmentStringsA);
    s.add("kernel32", "GetEnvironmentVariableA", Conv::Stdcall, 3, &shimGetEnvironmentVariableA);
    s.add("kernel32", "GetEnvironmentVariableW", Conv::Stdcall, 3, &shimGetEnvironmentVariableW);

    s.add("kernel32", "IsProcessorFeaturePresent", Conv::Stdcall, 1, &shimIsProcessorFeaturePresent);
    s.add("kernel32", "SetErrorMode", Conv::Stdcall, 1, &shimSetErrorMode);
    s.add("kernel32", "VirtualQuery", Conv::Stdcall, 3, &shimVirtualQuery);
    // ★ `InitializeSListHead` 已挪到 kernel32_sync.cpp（SList 属同步族；那里按 x86 布局清 8 字节）
    s.add("kernel32", "GetSystemTimePreciseAsFileTime", Conv::Stdcall, 1, &shimGetSystemTimePreciseAsFileTime);
    s.add("kernel32", "SetConsoleCtrlHandler", Conv::Stdcall, 2, &shimSetConsoleCtrlHandler);
    s.add("kernel32", "GetConsoleMode", Conv::Stdcall, 2, &shimGetConsoleMode);
    s.add("kernel32", "GetConsoleOutputCP", Conv::Stdcall, 0, &shimGetConsoleOutputCP);
    s.add("kernel32", "SetConsoleOutputCP", Conv::Stdcall, 1, &shimSetConsoleOutputCP);

    // ★ ole32 的三条 COM 内存 API 也在 ole32.dll 的导入表里 —— 与 kernel32 的
    //   `CoTaskMemAlloc/Free` 是**同一个 guest 堆**（README §2.6 的口径），所以都要注册。
    s.add("ole32", "CoTaskMemAlloc", Conv::Stdcall, 1, &shimCoTaskMemAlloc);
    s.add("ole32", "CoTaskMemFree", Conv::Stdcall, 1, &shimCoTaskMemFree);
    s.add("ole32", "CoInitialize", Conv::Stdcall, 1, &shimCoInitialize);
    s.add("ole32", "CoUninitialize", Conv::Stdcall, 0, &shimCoUninitialize);
    s.add("ole32", "CoFreeUnusedLibraries", Conv::Stdcall, 0, &shimVoidOk);
    s.add("ole32", "CoAddRefServerProcess", Conv::Stdcall, 0, &shimCoAddRefServerProcess);
    s.add("ole32", "CoReleaseServerProcess", Conv::Stdcall, 0, &shimCoAddRefServerProcess);

    // ★ SEH / C++ EH 的两个 OS 原语（`REQ-01M708DF24KY0QZNEHY66S0HYW`）：**已实现**。
    //   shim 只搬参数 + 把控制交给状态机（派发在 `Machine::pumpSeh`，块边界）。
    s.add("kernel32", "RaiseException", Conv::Stdcall, 4, &shimRaiseException);
    s.add("kernel32", "RtlUnwind", Conv::Stdcall, 4, &shimRtlUnwind);

    // 明确登记为缺口（⛔ 不假装成功）
    s.addUnimplemented("kernel32", "FindResourceA", "M2 资源族（用户 2026-11-04：资源等 M2；不属 M1）");
    s.addUnimplemented("kernel32", "SizeofResource", "M2 资源族（用户 2026-11-04：资源等 M2；不属 M1）");
    s.addUnimplemented("kernel32", "LoadResource", "M2 资源族（用户 2026-11-04：资源等 M2；不属 M1）");
    s.addUnimplemented("kernel32", "LockResource", "M2 资源族（用户 2026-11-04：资源等 M2；不属 M1）");
}

}  // namespace lift
