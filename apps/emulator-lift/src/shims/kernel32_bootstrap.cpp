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
// ⛔ 本单的边界（后续单 REQ-01M6KH7EPPNWJDHPDQ6REV2RRM）：**只保证 ASCII**。非 ASCII 的
//   CT_CTYPE1 需要一张字符类表（CP932 假名 / 全角 / CJK），这里**不拿 `w < 0x80` 猜**：
//   非 ASCII 位给 0 + 一次日志，真正的分类等那张表进来再做。
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
constexpr std::uint32_t kCtCtype2 = 2;  // CT_CTYPE2（未实现）
constexpr std::uint32_t kCtCtype3 = 4;  // CT_CTYPE3（未实现）

// ASCII 的 CT_CTYPE1 位（0x00–0x7F）。文档给的定点：'A'=0x0181、'a'=0x0182、'G'=0x0101、
// '0'=0x0084、' '=0x0048、'!'=0x0010。
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
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint16_t w = cpu.mem->rd16(src + i * 2u);
        std::uint16_t flags = 0;
        if (w < 0x80) {
            flags = ctype1Ascii(w);
        } else {
            ++unclassified;  // 非 ASCII：见上面的边界（⛔ 不猜）
        }
        cpu.mem->wr16(dst + i * 2u, flags);
    }
    if (unclassified != 0) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            shimLog("kernel32", "GetStringTypeW",
                    "非 ASCII 的 CT_CTYPE1 分类未实现（需要一张字符类表；本次 %u 个字符按 0 位返回）",
                    unclassified);
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
//     · 未实现的 `dwMapFlags`（SORTKEY / BYTEREV / HIRAGANA / KATAKANA / HALFWIDTH / FULLWIDTH …）
//       ⇒ 0 + ERROR_INVALID_FLAGS(1008)（⛔ 旧实现静默按"无映射"成功）；LOWERCASE 与 UPPERCASE
//       同时给是非法组合 ⇒ 同样是 1008。
// ⛔ 本单的边界（后续单 REQ-01M6KH7HC3ZP3M5RS8EJ3813WC）：只做 ASCII 的大小写；
//   CP932 假名 / 全角的映射要另一张表。
// ---------------------------------------------------------------------------------------
constexpr std::uint32_t kLcmapLowercase = 0x00000100;
constexpr std::uint32_t kLcmapUppercase = 0x00000200;
constexpr std::uint32_t kLcmapDoable = kLcmapLowercase | kLcmapUppercase;

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
    if (unsupported != 0 ||
        (flags & kLcmapDoable) == kLcmapDoable) {
        cpu.shim.last_error = kErrInvalidFlags;  // ERROR_INVALID_FLAGS
        shimLog("kernel32", "LCMapStringW",
                "dwMapFlags=0x%08X 里有未实现的位（0x%08X）或 LOWERCASE|UPPERCASE 同时给 ⇒ 返回 0"
                "（ERROR_INVALID_FLAGS）",
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
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint16_t w = cpu.mem->rd16(src + i * 2u);
        if (lower && w >= 'A' && w <= 'Z') w = static_cast<std::uint16_t>(w - 'A' + 'a');
        else if (upper && w >= 'a' && w <= 'z') w = static_cast<std::uint16_t>(w - 'a' + 'A');
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
// ---------------------------------------------------------------------------------------
void shimGetEnvironmentStringsW(X86& cpu, ShimCall& call) {
    // ★ 环境块由 `setupProcessStructures` 在 guest 里写好（确定性最小集合；A4 输入清单的一项）。
    //   只有它为空时才退回本地常量（纯档夹具 / 没走 prepare 的调用）。
    if (cpu.shim.env_ea != 0) {
        call.result = cpu.shim.env_ea;
        return;
    }
    // 一块 UTF-16 的 "NAME=VALUE\0...\0" + 结尾的额外 \0
    const std::uint32_t ea = 0x7FFD5000u;  // 已由 prepare() 预留？见下：这里按需提交
    if (!cpu.mem->committed(ea, 0x1000)) {
        if (!cpu.mem->commitZeroed(ea, 0x1000, RegionKind::Other, "environment")) {
            call.result = 0;
            return;
        }
    }
    static const char kEnv[] = "PATH=.\0TEMP=E:\\TMP\0\0";
    std::uint32_t off = 0;
    for (std::size_t i = 0; i < sizeof(kEnv); ++i) {
        cpu.mem->wr16(ea + off, static_cast<std::uint8_t>(kEnv[i]));
        off += 2;
    }
    call.result = ea;
}

void shimFreeEnvironmentStringsW(X86&, ShimCall& call) { call.result = 1; }

void shimGetEnvironmentVariableA(X86& cpu, ShimCall& call) {
    const std::string name = call.str(0);
    const std::uint32_t buf = call.u32(1);
    const std::uint32_t cap = call.u32(2);
    std::string value;
    if (equalsIgnoreCase(name, "PATH")) value = ".";
    if (equalsIgnoreCase(name, "TEMP")) value = "E:\\TMP";
    if (value.empty()) {
        call.result = 0;
        cpu.shim.last_error = 203;  // ERROR_ENVVAR_NOT_FOUND
        return;
    }
    if (cap < value.size() + 1) {
        call.result = static_cast<std::uint32_t>(value.size() + 1);
        return;
    }
    guestWrite(*cpu.mem, buf, value);
    call.result = static_cast<std::uint32_t>(value.size());
}

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

    s.add("kernel32", "GetEnvironmentStringsW", Conv::Stdcall, 0, &shimGetEnvironmentStringsW);
    s.add("kernel32", "FreeEnvironmentStringsW", Conv::Stdcall, 1, &shimFreeEnvironmentStringsW);
    s.add("kernel32", "GetEnvironmentVariableA", Conv::Stdcall, 3, &shimGetEnvironmentVariableA);

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

    // 明确登记为缺口（⛔ 不假装成功）
    s.addUnimplemented("kernel32", "RaiseException", "SEH 展开不在 M1（plan §9 R1）");
    s.addUnimplemented("kernel32", "RtlUnwind", "SEH 展开不在 M1（plan §9 R1）");
    s.addUnimplemented("kernel32", "FindResourceA", "M2 资源族（用户 2026-11-04：资源等 M2；不属 M1）");
    s.addUnimplemented("kernel32", "SizeofResource", "M2 资源族（用户 2026-11-04：资源等 M2；不属 M1）");
    s.addUnimplemented("kernel32", "LoadResource", "M2 资源族（用户 2026-11-04：资源等 M2；不属 M1）");
    s.addUnimplemented("kernel32", "LockResource", "M2 资源族（用户 2026-11-04：资源等 M2；不属 M1）");
}

}  // namespace lift
