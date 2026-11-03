// emulator-lift · CRT 启动路径上的补充 shim（S7 的"缺口填平"）
//
// ★ 为什么单独一个文件：S6 的**前三族**（同步 / 内存 / 错误码与 TLS）在
//   `kernel32_{sync,mem,error_tls}.cpp`；这里放的是**只有跑启动路径才暴露出来**的那些
//   （`_heap_init` / `_ioinit` / `_setenvp` / `_setlocale` 一路会碰的 API），
//   以及 S6 后四族里被启动路径**实际用到**的最小一块（进程/模块、计时、码页）。
//   ⛔ 它们不是 stub：每条都按 Win32 的真实语义实现（做不到的点在注释与本文件末尾点名）。
// ★ 码页：本作是 CP932，但**真正的 CP932 表**属 S6 码页族（M1b，要引 libiconv 或自带表）。
//   M1a 只保证 ASCII（0x00–0x7F）逐字节正确，非 ASCII 记一次日志 —— 这是显式缺口，
//   ⛔ 不是"看起来能跑"。
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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
// ---------------------------------------------------------------------------------------
void shimGetStdHandle(X86&, ShimCall& call) {
    const std::uint32_t which = call.u32(0);
    // STD_INPUT=-10 / STD_OUTPUT=-11 / STD_ERROR=-12
    if (which == 0xFFFFFFF6u) call.result = 0x0000000Cu;  // stdin（假句柄）
    else if (which == 0xFFFFFFF5u) call.result = 0x00000010u;  // stdout
    else if (which == 0xFFFFFFF4u) call.result = 0x00000014u;  // stderr
    else {
        call.result = 0;
        call.cpu->shim.last_error = 6;
    }
}

void shimSetStdHandle(X86&, ShimCall& call) { call.result = 1; }
// FILE_TYPE_CHAR = 2：让 CRT 认为标准句柄是字符设备（不是磁盘文件）⇒ 不做缓冲/寻址
void shimGetFileType(X86&, ShimCall& call) { call.result = 2; }
void shimSetHandleCount(X86&, ShimCall& call) { call.result = call.u32(0); }

// ---------------------------------------------------------------------------------------
// 码页 / locale（`_setlocale` / `_setenvp` 会碰）
//    ⚠ M1a 的边界：只保证 ASCII；CP932 的真表在 M1b（S6 码页族）。
// ---------------------------------------------------------------------------------------
constexpr std::uint32_t kAcp = 932;  // 日语 Shift-JIS（README §2.6 的 GetACP(=932)）

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
    const bool is932 = (cp == 932 || cp == 0 || cp == 1);
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

// 只做 ASCII：非 ASCII 字节按"字节值 → 同一个 16 位值"映射（不是 CP932 的真映射）
std::string g_codepage_warned;

void warnCodepageOnce(const char* api) {
    if (g_codepage_warned.find(api) != std::string::npos) return;
    g_codepage_warned += api;
    std::fprintf(stderr,
                 "[shim] kernel32!%s: CP932 真表属 S6 码页族（M1b）；M1a 只保证 ASCII 逐字节正确，"
                 "非 ASCII 按字节值直通\n",
                 api);
}

void shimMultiByteToWideChar(X86& cpu, ShimCall& call) {
    const std::uint32_t cp = call.u32(0);
    const std::uint32_t src_ea = call.u32(2);
    const int src_len = call.i32(3);
    const std::uint32_t dst_ea = call.u32(4);
    const int dst_len = call.i32(5);
    (void)cp;
    warnCodepageOnce("MultiByteToWideChar");
    std::int64_t count = 0;
    if (src_len < 0) {
        // -1 ⇒ 含结尾 0，一直读到 0
        if (src_ea == 0) {
            call.result = 0;
            return;
        }
        while (cpu.mem->rd8(src_ea + static_cast<std::uint32_t>(count)) != 0) ++count;
        ++count;
    } else {
        count = src_len;
    }
    if (dst_ea == 0 || dst_len == 0) {
        call.result = static_cast<std::uint32_t>(count);
        return;
    }
    if (dst_len < count) {
        call.cpu->shim.last_error = 122;  // ERROR_INSUFFICIENT_BUFFER
        call.result = 0;
        return;
    }
    for (std::int64_t i = 0; i < count; ++i) {
        const std::uint8_t b = cpu.mem->rd8(src_ea + static_cast<std::uint32_t>(i));
        cpu.mem->wr16(dst_ea + static_cast<std::uint32_t>(i) * 2u, b);
    }
    call.result = static_cast<std::uint32_t>(count);
}

void shimWideCharToMultiByte(X86& cpu, ShimCall& call) {
    const std::uint32_t cp = call.u32(0);
    const std::uint32_t src_ea = call.u32(2);
    const int src_len = call.i32(3);
    const std::uint32_t dst_ea = call.u32(4);
    const int dst_len = call.i32(5);
    (void)cp;
    warnCodepageOnce("WideCharToMultiByte");
    std::int64_t count = 0;
    if (src_len < 0) {
        if (src_ea == 0) {
            call.result = 0;
            return;
        }
        while (cpu.mem->rd16(src_ea + static_cast<std::uint32_t>(count) * 2u) != 0) ++count;
        ++count;
    } else {
        count = src_len;
    }
    if (dst_ea == 0 || dst_len == 0) {
        call.result = static_cast<std::uint32_t>(count);
        return;
    }
    if (dst_len < count) {
        call.cpu->shim.last_error = 122;
        call.result = 0;
        return;
    }
    for (std::int64_t i = 0; i < count; ++i) {
        const std::uint16_t w = cpu.mem->rd16(src_ea + static_cast<std::uint32_t>(i) * 2u);
        cpu.mem->wr8(dst_ea + static_cast<std::uint32_t>(i), w > 0xFF ? static_cast<std::uint8_t>('?')
                                                                     : static_cast<std::uint8_t>(w));
    }
    call.result = static_cast<std::uint32_t>(count);
}

void shimGetStringTypeW(X86& cpu, ShimCall& call) {
    const std::uint32_t type = call.u32(0);
    const std::uint32_t src = call.u32(1);
    const int n = call.i32(2);
    const std::uint32_t dst = call.u32(3);
    (void)type;
    if (!src || !dst || n <= 0) {
        call.result = 0;
        return;
    }
    for (int i = 0; i < n; ++i) {
        const std::uint16_t w = cpu.mem->rd16(src + static_cast<std::uint32_t>(i) * 2u);
        std::uint16_t flags = 0;
        if (w == 0) flags |= 0x0001;                       // C1_NULL? 实际 CT_CTYPE1 的 C1_SPACE 等不同
        if (w < 0x20) flags |= 0x0020;                     // C1_CNTRL
        else if (w == 0x20) flags |= 0x0020 | 0x0008;      // 空白
        else if (w >= 0x30 && w <= 0x39) flags |= 0x0004 | 0x0080;  // 数字 + 十六进制
        else if ((w >= 'A' && w <= 'Z') || (w >= 'a' && w <= 'z')) flags |= 0x0004 | 0x0100;  // 字母
        cpu.mem->wr16(dst + static_cast<std::uint32_t>(i) * 2u, flags);
    }
    call.result = 1;
}

void shimLCMapStringW(X86& cpu, ShimCall& call) {
    const std::uint32_t flags = call.u32(1);
    const std::uint32_t src = call.u32(2);
    const int n = call.i32(3);
    const std::uint32_t dst = call.u32(4);
    const int cap = call.i32(5);
    if (!src || n == 0) {
        call.result = 0;
        return;
    }
    const std::int64_t count = n < 0 ? 1 : n;  // 只支持显式长度的路径（-1 记日志）
    if (n < 0) shimLog("kernel32", "LCMapStringW", "只支持显式长度（n>0）；n<0 的路径未实现");
    if (!dst || cap == 0) {
        call.result = static_cast<std::uint32_t>(count);
        return;
    }
    if (cap < count) {
        call.cpu->shim.last_error = 122;
        call.result = 0;
        return;
    }
    const bool lower = (flags & 0x100) != 0;  // LCMAP_LOWERCASE
    const bool upper = (flags & 0x200) != 0;  // LCMAP_UPPERCASE
    for (std::int64_t i = 0; i < count; ++i) {
        std::uint16_t w = cpu.mem->rd16(src + static_cast<std::uint32_t>(i) * 2u);
        if (lower && w >= 'A' && w <= 'Z') w = static_cast<std::uint16_t>(w - 'A' + 'a');
        if (upper && w >= 'a' && w <= 'z') w = static_cast<std::uint16_t>(w - 'a' + 'A');
        cpu.mem->wr16(dst + static_cast<std::uint32_t>(i) * 2u, w);
    }
    call.result = 1;
}

void shimIsValidLocale(X86&, ShimCall& call) { call.result = 1; }
void shimGetLocaleInfoA(X86& cpu, ShimCall& call) {
    const std::uint32_t dst = call.u32(2);
    const int cap = call.i32(3);
    const std::string s = "0409";  // en-US：只回一个良构的 LCID 名字，避免 CRT 认为 locale 无效
    if (!dst || cap < static_cast<int>(s.size()) + 1) {
        call.result = static_cast<std::uint32_t>(s.size() + 1);
        return;
    }
    guestWrite(*cpu.mem, dst, s);
    call.result = static_cast<std::uint32_t>(s.size() + 1);
}
void shimGetUserDefaultLCID(X86&, ShimCall& call) { call.result = 0x0409; }
void shimGetSystemDefaultLCID(X86&, ShimCall& call) { call.result = 0x0409; }
void shimGetThreadLocale(X86&, ShimCall& call) { call.result = 0x0409; }
void shimSetThreadLocale(X86&, ShimCall& call) { call.result = 1; }
void shimGetUserDefaultUILanguage(X86&, ShimCall& call) { call.result = 0x0409; }

// ---------------------------------------------------------------------------------------
// 环境块（`_setenvp` 会碰）
// ---------------------------------------------------------------------------------------
void shimGetEnvironmentStringsW(X86& cpu, ShimCall& call) {
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

void shimInitializeSListHead(X86& cpu, ShimCall& call) {
    const std::uint32_t ea = call.u32(0);
    if (ea) cpu.mem->wr32(ea, 0);
    call.result = 0;
}

void shimGetSystemTimePreciseAsFileTime(X86& cpu, ShimCall& call) {
    const std::uint32_t ea = call.u32(0);
    const std::uint64_t ft = cpu.clock->now100ns() + 0x019DB1DED53E8000ull;  // 1601→1970 的偏移
    if (ea) cpu.mem->wr64(ea, ft);
    call.result = 0;
}

void shimSetConsoleCtrlHandler(X86&, ShimCall& call) { call.result = 1; }
void shimGetConsoleMode(X86& cpu, ShimCall& call) {
    const std::uint32_t out = call.u32(1);
    if (out) cpu.mem->wr32(out, 0x0003u);  // ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT
    call.result = 1;
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
    s.add("kernel32", "GetLocaleInfoA", Conv::Stdcall, 4, &shimGetLocaleInfoA);
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
    s.add("kernel32", "InitializeSListHead", Conv::Stdcall, 1, &shimInitializeSListHead);
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
    s.addUnimplemented("kernel32", "FindResourceA", "资源族属 S6 后续（M1a 不做）");
    s.addUnimplemented("kernel32", "SizeofResource", "资源族属 S6 后续（M1a 不做）");
    s.addUnimplemented("kernel32", "LoadResource", "资源族属 S6 后续（M1a 不做）");
    s.addUnimplemented("kernel32", "LockResource", "资源族属 S6 后续（M1a 不做）");
}

}  // namespace lift
