// emulator-lift · kernel32 进程 / 模块信息族（+ CRT 启动路径上会碰到的几条）
//
// ★ 这一族的口径：
//   · 模块句柄 = `cpu.shim.module_handle`（= image base，**不是**宿主模块句柄）；
//     exe 全路径 / 命令行在 guest 里各有一份（`cpu.shim.exe_path_ea` / `cmdline_ea`），由
//     `setupProcessStructures` 写好，这里只读它、不再拼字符串。
//   · `GetModuleHandleA/W(0)` 与"等于 exe 文件名（大小写不敏感、可省 `.exe`）"都返回主模块句柄；
//     其余按**"此刻在进程里吗"**回答（见下面"模块表"一节）：`ntdll` / `kernel32` + 本镜像**导入表**里
//     的模块 ⇒ 非 0 假句柄；其余 ⇒ 0 + `SetLastError(126)` + 一行日志。
//   · `GetProcAddress` 的导出真源 = **shim 注册表**（`ShimRegistry::find`），不是 PE 导出表：
//     句柄 → 模块 key → `find(key, name)` ⇒ 返回桩页里的唯一地址（找不到才 127）；
//     运行期导出（DDraw 的 `DirectDrawCreate` 之类）由 `ShimRegistry::addRuntime` 登记。
//   · `LoadLibraryA/W` 只回答"系统里有没有这个 DLL"（有就给假句柄）；它**不带来**导出 ——
//     某个导出能不能拿到，取决于有没有人用 `add` / `addStub` / `addRuntime` 登记过它。
//     —— 如果 CRT 启动真的调了它们，报告里必须看得见。
//   · `GetVersion` 报 **XP 5.1.2600**（这个 EXE 的年代口径）；`GetSystemInfo` 报单核 x86。
//
// ⚠ 偏差（逐条）：
//   1. **`GetCommandLineW` 返回的是 ANSI 缓冲区地址**（`cpu.shim.cmdline_ea`，与本批口径一致）。
//      guest 若真按 UTF-16 读它，看到的是隔 1 字节的 ASCII —— 影响：走 W 版命令行的 CRT 会解析错。
//      真机 `GetCommandLineW` 是独立的宽字符串。M1a 只保证 A 版可用（CRT 走 A 版）。
//   2. **W 版缓冲区的宽字符是"字节 → 16 位"扩展**（ASCII 直通、非 ASCII 变 `?`）。理由：guest 的
//      exe 路径 / 命令行来自 ANSI 缓冲，本模拟器不带 CP_ACP 转换表。影响：非 ASCII 路径（本仓
//      的游戏目录是日文名）在 W 版里会变成 `?`；A 版逐字节原样（freetype 之外没人读 W 版）。
//   3. **`GetStartupInfoA/W` 只填 cb 与全零**：`dwFlags`/标准句柄/`lpDesktop` 都是 0。真机由
//      loader 填。影响：guest 若检查 `STARTF_USESTDHANDLES` 会看到未设置（CRT 不需要它）。
//   4. **`GetVersionExA` 不写 `dwOSVersionInfoSize`**（保持调用方给的值），且 `szCSDVersion` 全 0；
//      `dwOSVersionInfoSize < 0x14` 时按 Windows 返回 FALSE + `ERROR_INSUFFICIENT_BUFFER(122)`。
//   5. **`GetSystemInfo` 的 `dwActiveProcessorMask` 留 0**（按本批口径"其余 0"）。真机是 1。
//      影响：读这个掩码的代码会看到"没有处理器"——CRT / 引擎都不读它；若将来有代码读，改这一处。
//   6. **`TerminateProcess` 不区分目标进程**：任何句柄都让 guest 停下（本模拟器只有一个进程）。
//   7. **`FreeLibrary` 恒 TRUE、`DisableThreadLibraryCalls` 恒 TRUE**：本模拟器没有真模块加载，
//      这两个调用没有可撤销的状态（不是"没做却假装"——它们是幂等的无害操作）。
//   8. **`GetSystemDirectoryA` / `GetWindowsDirectoryA` / `GetFullPathNameA` / `GetModuleHandleExA/W`
//      已在 S6 落地**（前者在本文件的模块段，后两者在 kernel32_file.cpp / 本文件的模块段）：
//      系统目录是**固定的 guest 路径** `C:\WINDOWS` / `C:\WINDOWS\SYSTEM32`（⛔ 不暴露宿主路径）；
//      `GetModuleHandleEx` 的引用计数不建模（见该函数的注释）。
//   9. **系统模块的句柄是确定性假值**（`0x7F000000 + 表内序号*64KiB`）：本模拟器不真的装载
//      系统 DLL，但 `ntdll` / `kernel32` / 导入表里的模块在真机上**确实在进程里** ⇒ 返回 0 是假失败
//      （实测：CRT 的 `__scrt_initialize_fls` 因此返回 false ⇒ `_fast_error_exit` ⇒ 到不了 WinMain）。
//      影响：这些句柄只能当**不透明令牌**用（比较 / 传给 `GetProcAddress`）；⛔ 不要拿它去读 PE 头。
//      "模块存在"并不意味着"它的每个导出都可用"：导出真源是 shim 注册表（见 `GetProcAddress`）。
//      ★ **例外：AGERC.DLL**（2026-11-04 用户改口径）是**真实映射进 guest 的模块**：句柄 = 真实
//        ImageBase（`cpu.shim.agerc_base`），PE 头 / 节 / 资源都在 guest 里；没有镜像就装不上（126）。
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include "runtime/cp932.h"
#include "runtime/image.h"
#include "runtime/sha256.h"
#include "runtime/vfs.h"
#include "runtime/x86.h"
#include "shims/register.h"

namespace lift {
namespace {

constexpr std::uint32_t kErrInvalidHandle = 6u;
constexpr std::uint32_t kErrInvalidParameter = 87u;
constexpr std::uint32_t kErrBufferOverflow = 111u;
constexpr std::uint32_t kErrInsufficientBuffer = 122u;
constexpr std::uint32_t kErrModNotFound = 126u;
constexpr std::uint32_t kErrProcNotFound = 127u;

constexpr std::uint32_t kStartupInfoASize = 0x44u;  // sizeof(STARTUPINFOA) = 68
// ★ sizeof(STARTUPINFOW) **也是** 0x44：W 版只是把 3 个 `LPSTR` 换成 `LPWSTR`（指针宽度不变），
//   其余 12 个 DWORD / WORD / HANDLE 字段一模一样 ⇒ 结构体大小与 A 版**完全相同**。
//   这里曾写成 0x68（104）—— 那会让 shim 的 `fillBytes` 越过调用方的局部变量 36 字节，
//   实测把 `__ioinit`（0x504233）的**返回地址**清零 ⇒ `ret` 回 0 ⇒ decode-error @ 00000000。
constexpr std::uint32_t kStartupInfoWSize = 0x44u;  // sizeof(STARTUPINFOW) = 68
constexpr std::uint32_t kSystemInfoSize = 0x24u;

constexpr std::uint32_t kWin32Nt = 2u;  // VER_PLATFORM_WIN32_NT

void requireMem(X86& cpu, const char* api) {
    if (!cpu.mem) shimAbort("kernel32", api, "cpu.mem 为空（Machine 没装配 guest 内存）");
}

std::string asciiLower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    return s;
}

std::string baseName(const std::string& path) {
    const auto pos = path.find_last_of("\\/");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

void stripExeSuffix(std::string& s) {
    if (s.size() > 4 && s.compare(s.size() - 4, 4, ".exe") == 0) {
        s.erase(s.size() - 4);
    }
}

std::string narrowAscii(const std::u16string& w) {
    std::string s;
    s.reserve(w.size());
    for (const char16_t c : w) {
        s.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    }
    return s;
}

// guest 里的 exe 全路径（没有 ⇒ 空串；调用方决定是失败还是打日志）
std::string exePath(X86& cpu) {
    if (!cpu.mem || cpu.shim.exe_path_ea == 0) return std::string();
    return cpu.mem->readCString(cpu.shim.exe_path_ea);
}

// `name` 是否指主模块：全路径相等，或文件名相等（大小写不敏感、可省 `.exe`）
bool moduleNameMatches(const std::string& want, const std::string& exe_path) {
    if (exe_path.empty()) return false;
    if (asciiLower(want) == asciiLower(exe_path)) return true;
    std::string a = asciiLower(baseName(want));
    std::string b = asciiLower(baseName(exe_path));
    stripExeSuffix(a);
    stripExeSuffix(b);
    return a == b;
}

void requireBuffer(X86& cpu, std::uint32_t ea, std::uint32_t cap, const char* api,
                   const char* what) {
    if (ea == 0 || cap == 0) {
        shimAbort("kernel32", api, "%s 为空（0x%08X）或容量为 0 —— 调用方传了非法缓冲区", what, ea);
    }
    (void)cpu;
}

// 出参缓冲区必须是**已提交**的 guest 内存（⛔ 不静默写坏）
void requireCommitted(X86& cpu, std::uint32_t ea, std::size_t n, const char* api, const char* what) {
    if (ea == 0) shimAbort("kernel32", api, "%s 为 NULL", what);
    if (n == 0) return;
    if (!cpu.mem->committed(ea, n)) {
        shimAbort("kernel32", api, "%s = 0x%08X 起 %u 字节未提交（guest 缓冲区非法）", what, ea,
                  static_cast<unsigned>(n));
    }
}

// 把 ANSI 串写进 guest 缓冲：截断到 cap-1 并保证 0 结尾，返回**写入的字节数**（不含结尾 0）
std::uint32_t writeAnsi(X86& cpu, std::uint32_t ea, std::uint32_t cap, const std::string& s) {
    const std::size_t n = std::min<std::size_t>(s.size(), static_cast<std::size_t>(cap) - 1);
    if (n > 0) cpu.mem->writeBytes(ea, s.data(), n);
    cpu.mem->wr8(ea + static_cast<std::uint32_t>(n), 0);
    return static_cast<std::uint32_t>(n);
}

// 把 ANSI（CP932）串写进 guest 的 UTF-16 缓冲：真 CP932→UTF-16（S6 码页族；⛔ 不是字节直通）
std::uint32_t writeWide(X86& cpu, std::uint32_t ea, std::uint32_t cap_chars, const std::string& s) {
    const std::u16string w = cp932ToWide(s);
    const std::size_t n = std::min<std::size_t>(w.size(), static_cast<std::size_t>(cap_chars) - 1);
    for (std::size_t i = 0; i < n; ++i) {
        cpu.mem->wr16(ea + static_cast<std::uint32_t>(i) * 2u, static_cast<std::uint16_t>(w[i]));
    }
    cpu.mem->wr16(ea + static_cast<std::uint32_t>(n) * 2u, 0);
    return static_cast<std::uint32_t>(n);
}

// ---- 模块表 ----
// ★ 两个问题必须分开回答（混在一起就会给出真机不会给的值）：
//   ① `GetModuleHandle` 问的是"**这个模块此刻在进程里吗**"；
//   ② `LoadLibrary` 问的是"**系统里有这个 DLL 吗**"（有就能装载）。
//   把①按②回答会出错：例 `mscoree.dll` 系统里一定有，但本 EXE 没导入也没 LoadLibrary 过
//   ⇒ 真机 `GetModuleHandleW(L"mscoree.dll")` 返回 **0**（CRT 的 `_exit` 路径正是靠这个 0 跳过
//   `CorExitProcess`）。所以①的判据是"loader 在入口前装载过的那些"：
//     · 主模块（image base）；
//     · `ntdll` / `kernel32` —— 每个 Win32 进程都有（本 EXE 的导入表里没有它们，
//       但 `GetModuleHandleW(L"KERNEL32.DLL")` 在真机上一定非 0）；
//     · **本镜像导入表里的模块**（loader 在 entry 之前全部装载）。`ShimDef::imported` 就是这张表。
//   ★ 实测意义：CRT 启动的 `__scrt_initialize_fls`（0x4F0CE5）先 `GetModuleHandleW(L"KERNEL32.DLL")`
//     （串在 .data:005303B0），拿 0 ⇒ 返回 false ⇒ `_fast_error_exit(0x10)` ⇒ `_exit(255)`
//     ⇒ 永远跑不到 `_WinMain@16`。返回非 0 是**真机事实**，不是为了让谁过去。
// ⛔ 导出**不是**由"模块在不在"决定的：`GetProcAddress` 查的是 shim 注册表（可能 127）——
//    "模块存在、这个函数没登记"是 M1/M2 的真实边界，日志必须分开说。
constexpr std::uint32_t kFakeModuleBase = 0x7F000000u;
constexpr std::uint32_t kFakeModuleStride = 0x10000u;

// `LoadLibraryA/W` 的"系统里存在"表：真机上这些 DLL 都能装载成功。
const char* const kLoadableModules[] = {
    "ntdll.dll",    "kernel32.dll", "user32.dll",   "gdi32.dll",     "advapi32.dll",
    "winmm.dll",    "ole32.dll",    "oleaut32.dll", "shell32.dll",   "comctl32.dll",
    "version.dll",  "imm32.dll",    "ddraw.dll",    "d3d9.dll",      "d3dx9_43.dll",
    "ws2_32.dll",   "imagehlp.dll", "dxva2.dll",    "wininet.dll",   "mscoree.dll",
    "shlwapi.dll",  "mf.dll",       "mfplat.dll",   "evr.dll",
    // ★ AGERC.DLL：本作的设置对话框（用户 2026-11-04 改口径：**真实 guest 模块**）。
    //   它**不在**导入表里（guest 用 LoadLibrary 动态装载）⇒ 这里只回答"系统里能装载"；
    //   **句柄是它真实的模块基址**（`cpu.shim.agerc_base`），⛔ 不再是 0x7F18xxxx 假句柄。
    //   没有镜像（没给 --agerc 且 --game-dir 下没有 AGERC.DLL）⇒ LoadLibrary 返回 0 + 126。
    "agerc.dll",
};

// 模块名归一化：去路径 + 小写 + 去尾部 `.dll`（`KERNEL32.DLL` / `kernel32` 归一化后相等）
std::string moduleKey(std::string s) {
    s = asciiLower(baseName(s));
    if (s.size() > 4 && s.compare(s.size() - 4, 4, ".dll") == 0) s.resize(s.size() - 4);
    return s;
}

// 假句柄：`0x7F000000 + 序号 * 64 KiB`。
// ★ 序号取"该模块在**模块表里的去重序号**"（见 `moduleKeyTable`），⛔ 不用名字哈希：
//   哈希会**撞**（两个不同模块拿到同一个句柄），而 guest 会拿句柄比较。
// ★ 序号是纯整数、与运行顺序无关 ⇒ 确定性（判据 A4 的前提）；句柄恒 < 0x80000000，
//   且落在 0x7F000000..0x7FEF0000 —— 与镜像 / 堆 / 栈 / TEB / 桩页（见 GuestLayout）都不重叠。
std::uint32_t moduleHandleForIndex(std::size_t index) {
    return kFakeModuleBase + static_cast<std::uint32_t>(index) * kFakeModuleStride;
}

// 模块表（去重、稳定顺序）：
//   ① 本镜像导入表里的模块（loader 在 entry 之前全部装载；`ShimDef::imported` 就是这张表）
//   ② `kernel32` / `ntdll`（每个 Win32 进程都有；本 EXE 的导入表里没有它们）
//   ③ 其余"系统里一定有的"（只对 `LoadLibrary` 有意义）
// ⇒ 同一模块在 `GetModuleHandle` 与 `LoadLibrary` 里**拿到同一个句柄**。
std::vector<std::string> moduleKeyTable(const X86& cpu) {
    std::vector<std::string> keys;
    auto add = [&keys](const std::string& k) {
        if (std::find(keys.begin(), keys.end(), k) == keys.end()) keys.push_back(k);
    };
    if (cpu.shims) {
        for (const ShimDef& d : cpu.shims->all()) {
            if (d.imported) add(moduleKey(d.module));
        }
    }
    add("kernel32");
    add("ntdll");
    for (const char* k : kLoadableModules) add(moduleKey(k));
    return keys;
}

// 在表里的序号 → 句柄；0 = 不在表里 / 没有 AGERC 镜像。
// ★ AGERC 是**唯一**一个真实装载的模块：句柄 = 真实模块基址（`cpu.shim.agerc_base`），
//   ⛔ 不参与下面那套确定性假句柄编号；没有镜像 ⇒ 0（调用方按"不认得 / 未装载"处理）。
std::uint32_t moduleHandleIn(const X86& cpu, const std::vector<std::string>& table,
                             const std::string& name) {
    const std::string want = moduleKey(name);
    if (want == "agerc") return cpu.shim.agerc_base;
    for (std::size_t i = 0; i < table.size(); ++i)
        if (table[i] == want) return moduleHandleForIndex(i);
    return 0;
}

bool isLoadableModule(const std::string& name) {
    const std::string want = moduleKey(name);
    for (const char* k : kLoadableModules)
        if (want == moduleKey(k)) return true;
    return false;
}

// "这个模块此刻在**进程里**吗"（≠ "系统里有"）：导入表里的 + kernel32/ntdll
bool moduleIsLoaded(const X86& cpu, const std::string& key) {
    if (key == "kernel32" || key == "ntdll") return true;
    // AGERC 是**真实装载的 guest 模块**：镜像在（agerc_base 非 0）且 guest 调过 LoadLibrary
    // 才"在进程里"（真机：AGERC.DLL 不在导入表里，入口前 loader 不会装它）。
    if (key == "agerc") return cpu.shim.agerc_loaded && cpu.shim.agerc_base != 0;
    if (!cpu.shims) return false;
    for (const ShimDef& d : cpu.shims->all())
        if (d.imported && moduleKey(d.module) == key) return true;
    return false;
}

// ---- 模块 ----
// 名字 → 模块句柄（0 = 没装载）。`GetModuleHandle*` 与 `GetModuleHandleEx*` 共用这一份判据
// （"**此刻在进程里吗**"，⛔ 不是"系统里有"）。
std::uint32_t lookupModuleHandle(X86& cpu, const std::string& want, const char* api) {
    const std::string exe = exePath(cpu);
    if (moduleNameMatches(want, exe)) return cpu.shim.module_handle;
    const std::string key = moduleKey(want);
    if (!moduleIsLoaded(cpu, key)) return 0;
    const std::uint32_t handle = moduleHandleIn(cpu, moduleKeyTable(cpu), want);
    if (handle != 0) {
        if (key == "agerc") {
            shimLog("kernel32", api,
                    "\"%s\" 已装载 ⇒ **真实 AGERC 模块基址 0x%08X**（HINSTANCE；导出由 shim 注册表决定）",
                    want.c_str(), handle);
        } else {
            shimLog("kernel32", api,
                    "\"%s\" 已装载 ⇒ 假句柄 0x%08X（导出由 shim 注册表决定，见 GetProcAddress）",
                    want.c_str(), handle);
        }
    }
    return handle;
}

void getModuleHandle(X86& cpu, ShimCall& call, bool wide) {
    const char* api = wide ? "GetModuleHandleW" : "GetModuleHandleA";
    requireMem(cpu, api);
    const std::uint32_t name_ea = call.u32(0);
    if (name_ea == 0) {
        call.result = cpu.shim.module_handle;
        return;
    }
    const std::string want = wide ? narrowAscii(call.wstr(0)) : call.str(0);
    const std::uint32_t handle = lookupModuleHandle(cpu, want, api);
    if (handle == 0) {
        // ★ 判据是"**已装载**"（导入表里的 + kernel32/ntdll），⛔ 不是"系统里有"：
        //   例 `mscoree.dll` 系统里有、但本进程没装载 ⇒ 真机返回 0，这里也必须返回 0。
        cpu.shim.last_error = kErrModNotFound;
        if (moduleKey(want) == "agerc") {
            if (cpu.shim.agerc_base == 0) {
                shimLog("kernel32", api,
                        "AGERC 没有装载到 guest（没有 AGERC 镜像：--agerc / --game-dir 下的 AGERC.DLL）"
                        " ⇒ 返回 0 + 错误 126");
            } else {
                shimLog("kernel32", api,
                        "AGERC 镜像已映射在 0x%08X，但 guest 还没调 LoadLibrary(\"AGERC.DLL\")"
                        " ⇒ 返回 0 + 错误 126（真机同：入口前 loader 不装它）",
                        cpu.shim.agerc_base);
            }
        } else {
            const std::string exe = exePath(cpu);
            shimLog("kernel32", api, "主模块是 \"%s\"；\"%s\" 未装载 ⇒ 返回 0 + 错误 126",
                    exe.empty() ? "<未设置>" : exe.c_str(), want.c_str());
        }
    }
    call.result = handle;
}

void shimGetModuleHandleA(X86& cpu, ShimCall& call) { getModuleHandle(cpu, call, false); }
void shimGetModuleHandleW(X86& cpu, ShimCall& call) { getModuleHandle(cpu, call, true); }

// GetModuleHandleExA/W：文档的三个 flag 与两条硬约束
//   · FROM_ADDRESS(0x4)：lpModuleName 是**模块内的一个地址** ⇒ 反查主模块（本模拟器只给主模块
//     真实的地址范围；其它模块是确定性假句柄，没有映射）；
//   · PIN(0x1) 与 UNCHANGED_REFCOUNT(0x2) **不能同时用** ⇒ ERROR_INVALID_PARAMETER；
//   · UNCHANGED_REFCOUNT ⇒ 行为等价于 GetModuleHandle（不取引用）；
//   · 其它情况"增加引用计数" —— 本模拟器**不建模引用计数**（FreeLibrary 恒 TRUE），只记一行。
void getModuleHandleEx(X86& cpu, ShimCall& call, bool wide) {
    const char* api = wide ? "GetModuleHandleExW" : "GetModuleHandleExA";
    requireMem(cpu, api);
    constexpr std::uint32_t kPin = 0x00000001u;
    constexpr std::uint32_t kUnchangedRefcount = 0x00000002u;
    constexpr std::uint32_t kFromAddress = 0x00000004u;
    const std::uint32_t flags = call.u32(0);
    const std::uint32_t name_ea = call.u32(1);
    const std::uint32_t out_ea = call.u32(2);
    if (out_ea == 0) {
        cpu.shim.last_error = kErrInvalidParameter;
        shimLog("kernel32", api, "phModule 为 NULL ⇒ 返回 0 + 错误 87");
        call.result = 0;
        return;
    }
    requireCommitted(cpu, out_ea, 4, api, "phModule");
    cpu.mem->wr32(out_ea, 0);  // 文档：失败时 *phModule 为 NULL
    if ((flags & (kPin | kUnchangedRefcount)) == (kPin | kUnchangedRefcount)) {
        cpu.shim.last_error = kErrInvalidParameter;
        shimLog("kernel32", api, "PIN 与 UNCHANGED_REFCOUNT 不能同时用 ⇒ 返回 0 + 错误 87");
        call.result = 0;
        return;
    }
    if ((flags & ~(kPin | kUnchangedRefcount | kFromAddress)) != 0) {
        cpu.shim.last_error = kErrInvalidParameter;
        shimLog("kernel32", api, "未知 flag 0x%08X ⇒ 返回 0 + 错误 87", flags);
        call.result = 0;
        return;
    }

    std::uint32_t handle = 0;
    if ((flags & kFromAddress) != 0) {
        if (name_ea == 0) {
            cpu.shim.last_error = kErrInvalidParameter;
            shimLog("kernel32", api, "FROM_ADDRESS 但 lpModuleName 为 NULL ⇒ 返回 0 + 错误 87");
            call.result = 0;
            return;
        }
        const std::uint32_t addr = name_ea;
        const std::uint32_t base = cpu.shim.module_handle;
        const std::uint32_t size = cpu.shim.module_size;
        // ★ AGERC 是真实映射的镜像 ⇒ 它覆盖的地址也能反查到自己的基址（真机 loader 行为）。
        const std::uint32_t agerc_size =
            cpu.shim.agerc_image ? cpu.shim.agerc_image->size_of_image : 0u;
        if (base != 0 && addr >= base && (size == 0 || addr < base + size)) {
            handle = base;
        } else if (cpu.shim.agerc_base != 0 && addr >= cpu.shim.agerc_base &&
                   (agerc_size == 0 || addr - cpu.shim.agerc_base < agerc_size)) {
            handle = cpu.shim.agerc_base;
        } else {
            // 文档没点名"地址不在任何模块里"用哪个错误码 ⇒ 取 ERROR_MOD_NOT_FOUND（"找不到模块"）
            cpu.shim.last_error = kErrModNotFound;
            shimLog("kernel32", api, "地址 0x%08X 不在任何已装载模块里 ⇒ 返回 0 + 错误 126", addr);
            call.result = 0;
            return;
        }
    } else if (name_ea == 0) {
        handle = cpu.shim.module_handle;  // 文档：lpModuleName 为 NULL ⇒ 本进程的 exe
    } else {
        const std::string want = wide ? narrowAscii(call.wstr(1)) : call.str(1);
        handle = lookupModuleHandle(cpu, want, api);
    }

    if (handle == 0) {
        cpu.shim.last_error = kErrModNotFound;
        shimLog("kernel32", api, "找不到模块 ⇒ 返回 0 + 错误 126");
        call.result = 0;
        return;
    }
    if ((flags & kUnchangedRefcount) == 0) {
        shimLog("kernel32", api,
                "模块 0x%08X 的引用计数不建模（FreeLibrary 恒 TRUE）⇒ 只回句柄", handle);
    }
    cpu.mem->wr32(out_ea, handle);
    call.result = 1;
}

void shimGetModuleHandleExA(X86& cpu, ShimCall& call) { getModuleHandleEx(cpu, call, false); }
void shimGetModuleHandleExW(X86& cpu, ShimCall& call) { getModuleHandleEx(cpu, call, true); }

void getModuleFileName(X86& cpu, ShimCall& call, bool wide) {
    const char* api = wide ? "GetModuleFileNameW" : "GetModuleFileNameA";
    requireMem(cpu, api);
    const std::uint32_t hmod = call.u32(0);
    const std::uint32_t buf = call.u32(1);
    const std::uint32_t cap = call.u32(2);
    if (hmod != 0 && hmod != cpu.shim.module_handle) {
        cpu.shim.last_error = kErrModNotFound;
        shimLog("kernel32", api, "hModule=0x%08X 不是主模块（0x%08X）⇒ 返回 0", hmod,
                cpu.shim.module_handle);
        call.result = 0;
        return;
    }
    requireBuffer(cpu, buf, cap, api, "lpFilename");
    std::string path = exePath(cpu);
    if (path.empty()) {
        cpu.shim.last_error = kErrInvalidParameter;
        shimLog("kernel32", api, "guest 里没有 exe 全路径（exe_path_ea == 0）⇒ 返回 0");
        call.result = 0;
        return;
    }
    // ★ S6：模块文件名的**guest 视图**由 VFS 反解（`--game-dir` 是唯一根）——
    //   guest 看到的是它自己的路径，⛔ 不是宿主的仓库路径（那会让 guest 之后的相对路径解析错）。
    if (cpu.vfs) {
        std::string guest;
        if (cpu.vfs->toGuest(path, &guest)) {
            path = guest;
        } else if (!cpu.vfs->guestRoot().empty()) {
            // 执行源在 `--game-dir` 之外（本仓的 corpus/）⇒ guest 视图 = 安装根 + 文件名。
            // 这是**主模块的位置**的确定性表述；宿主路径只留在 trace 输入清单里。
            path = cpu.vfs->guestRoot() + "\\" + baseName(path);
        }
    }
    call.result = wide ? writeWide(cpu, buf, cap, path) : writeAnsi(cpu, buf, cap, path);
}

void shimGetModuleFileNameA(X86& cpu, ShimCall& call) { getModuleFileName(cpu, call, false); }
void shimGetModuleFileNameW(X86& cpu, ShimCall& call) { getModuleFileName(cpu, call, true); }

// ---- 命令行 / 启动信息 ----
void shimGetCommandLineA(X86& cpu, ShimCall& call) {
    if (cpu.shim.cmdline_ea == 0) {
        shimLog("kernel32", "GetCommandLineA", "guest 里没有命令行（cmdline_ea == 0）⇒ 返回 0");
    }
    call.result = cpu.shim.cmdline_ea;
}

void shimGetCommandLineW(X86& cpu, ShimCall& call) {
    // 偏差 1：返回的还是那个 ANSI 缓冲区（本批不生成宽字符命令行）
    if (cpu.shim.cmdline_ea == 0) {
        shimLog("kernel32", "GetCommandLineW", "guest 里没有命令行（cmdline_ea == 0）⇒ 返回 0");
    }
    call.result = cpu.shim.cmdline_ea;
}

void shimGetStartupInfoA(X86& cpu, ShimCall& call) {
    requireMem(cpu, "GetStartupInfoA");
    const std::uint32_t ea = call.u32(0);
    if (ea == 0) shimAbort("kernel32", "GetStartupInfoA", "lpStartupInfo 为 NULL");
    cpu.mem->fillBytes(ea, 0, kStartupInfoASize);
    cpu.mem->wr32(ea, kStartupInfoASize);  // cb
    call.result = 0;                       // void
}

void shimGetStartupInfoW(X86& cpu, ShimCall& call) {
    requireMem(cpu, "GetStartupInfoW");
    const std::uint32_t ea = call.u32(0);
    if (ea == 0) shimAbort("kernel32", "GetStartupInfoW", "lpStartupInfo 为 NULL");
    cpu.mem->fillBytes(ea, 0, kStartupInfoWSize);
    cpu.mem->wr32(ea, kStartupInfoWSize);
    call.result = 0;
}

// ---- 版本 ----
void shimGetVersion(X86& cpu, ShimCall& call) {
    (void)cpu;
    // 高字节 0（NT 系）| build 2600 (0xA28) | minor 1 | major 5
    call.result = 0x0A280105u;
}

// ★ `GetVersionExA` 与 `GetVersionExW` **同一份实现**（任务书点名"与 A 版同源"）。
//   理由：OSVERSIONINFOW 与 OSVERSIONINFOA 的**字段布局完全相同**（三个 DWORD + 平台 DWORD +
//   TCHAR[128]；A/W 只差字符串元素的宽度），而 szCSDVersion 在我们的实现里恒为全 0
//   ⇒ 两者的写入逐字节相同。⛔ 不要另抄一份（抄了必然漂）。
void getVersionEx(X86& cpu, ShimCall& call, const char* api) {
    requireMem(cpu, api);
    const std::uint32_t ea = call.u32(0);
    if (ea == 0) shimAbort("kernel32", api, "lpVersionInformation 为 NULL");
    const std::uint32_t size = cpu.mem->rd32(ea);  // 保持调用方给的值（不覆盖）
    if (size < 0x14) {
        cpu.shim.last_error = kErrInsufficientBuffer;
        shimLog("kernel32", api, "dwOSVersionInfoSize=%u 太小（< 0x14）⇒ 返回 0", size);
        call.result = 0;
        return;
    }
    cpu.mem->wr32(ea + 0x04, 5u);        // dwMajorVersion
    cpu.mem->wr32(ea + 0x08, 1u);        // dwMinorVersion
    cpu.mem->wr32(ea + 0x0C, 2600u);     // dwBuildNumber
    cpu.mem->wr32(ea + 0x10, kWin32Nt);  // dwPlatformId
    // szCSDVersion[128] +（若调用方给的是 OSVERSIONINFOEX 大小的结构）扩展字段：
    // 把 [0x14, min(size, 0x11C)) 全清 0 ⇒ CSD 串为空、wServicePackMajor..wSuiteMask 全 0。
    // ⛔ 不越过调用方声明的 size（那是 guest 的栈/堆）。
    const std::uint32_t clear_end = std::min<std::uint32_t>(size, 0x11Cu);
    if (clear_end > 0x14u) cpu.mem->fillBytes(ea + 0x14, 0, clear_end - 0x14u);
    call.result = 1;
}

void shimGetVersionExA(X86& cpu, ShimCall& call) { getVersionEx(cpu, call, "GetVersionExA"); }
void shimGetVersionExW(X86& cpu, ShimCall& call) { getVersionEx(cpu, call, "GetVersionExW"); }

// ---- 计算机名（固定值；⛔ 不暴露宿主名）----
// 文档：成功 ⇒ *nSize = 拷贝的字符数（**不含**结尾 0）；缓冲太小 ⇒ FALSE +
// ERROR_BUFFER_OVERFLOW(111)，*nSize = 所需大小（**含**结尾 0）。
constexpr const char* kGuestComputerName = "AGE-LIFT";

void shimGetComputerNameA(X86& cpu, ShimCall& call) {
    const char* api = "GetComputerNameA";
    requireMem(cpu, api);
    const std::uint32_t buf = call.u32(0);
    const std::uint32_t size_ea = call.u32(1);
    if (buf == 0 || size_ea == 0) {
        cpu.shim.last_error = kErrInvalidParameter;
        shimLog("kernel32", api, "lpBuffer / nSize 为 NULL ⇒ 返回 0 + 错误 87");
        call.result = 0;
        return;
    }
    requireCommitted(cpu, size_ea, 4, api, "nSize");
    const std::uint32_t cap = cpu.mem->rd32(size_ea);
    const std::string name = kGuestComputerName;
    const std::uint32_t need = static_cast<std::uint32_t>(name.size()) + 1u;
    if (cap < need) {
        cpu.mem->wr32(size_ea, need);  // 文档：告诉调用方需要多大（含结尾 0）
        cpu.shim.last_error = kErrBufferOverflow;
        shimLog("kernel32", api, "缓冲 %u 字节太小（需要 %u）⇒ 返回 0 + 错误 111", cap, need);
        call.result = 0;
        return;
    }
    writeAnsi(cpu, buf, cap, name);
    cpu.mem->wr32(size_ea, static_cast<std::uint32_t>(name.size()));  // 不含结尾 0
    call.result = 1;
}

// ---- 整数乘除（MulDiv）----
// 文档：64 位乘积 / 第三个参数，结果**四舍五入**（正半整数向上、负半整数向下 ⇒ 远离零）；
//       溢出或分母为 0 ⇒ 返回 -1。⛔ 不要用浮点（53 位尾数会在 |乘积| > 2^53 时错位）。
void shimMulDiv(X86& cpu, ShimCall& call) {
    (void)cpu;
    const std::int32_t number = call.i32(0);
    const std::int32_t numerator = call.i32(1);
    const std::int32_t denominator = call.i32(2);
    if (denominator == 0) {
        call.result = 0xFFFFFFFFu;  // -1
        return;
    }
    const std::int64_t product = static_cast<std::int64_t>(number) * static_cast<std::int64_t>(numerator);
    const std::int64_t den = static_cast<std::int64_t>(denominator);
    std::int64_t result = product / den;      // 截断
    const std::int64_t rem = product % den;   // 符号随 product
    if (rem != 0) {
        // |rem|/|den| >= 0.5 ⇒ 向**远离零**的方向进一位
        const std::int64_t abs_rem = rem < 0 ? -rem : rem;
        const std::int64_t abs_den = den < 0 ? -den : den;
        if (2 * abs_rem >= abs_den) {
            result += ((product < 0) == (den < 0)) ? 1 : -1;
        }
    }
    if (result < -2147483647ll - 1ll || result > 2147483647ll) {
        call.result = 0xFFFFFFFFu;  // 溢出 ⇒ -1
        return;
    }
    call.result = static_cast<std::uint32_t>(static_cast<std::int32_t>(result));
}

// ---- 控制台 / 调试输出 ----
// 本模拟器的"控制台"只有 `GetStdHandle` 给的那三个伪句柄（见 kernel32_bootstrap.cpp）
constexpr std::uint32_t kStdInHandle = 0x0000000Cu;
constexpr std::uint32_t kStdOutHandle = 0x00000010u;
constexpr std::uint32_t kStdErrHandle = 0x00000014u;

bool isConsoleHandle(std::uint32_t h) {
    return h == kStdInHandle || h == kStdOutHandle || h == kStdErrHandle;
}

void shimGetConsoleCP(X86& cpu, ShimCall& call) {
    (void)cpu;
    // 控制台输入码页：与 GetACP 同源（本作是 CP932）
    call.result = 932u;
}

void shimWriteConsoleW(X86& cpu, ShimCall& call) {
    const char* api = "WriteConsoleW";
    requireMem(cpu, api);
    const std::uint32_t h = call.u32(0);
    const std::uint32_t buf = call.u32(1);
    const std::uint32_t nchars = call.u32(2);
    const std::uint32_t written_ea = call.u32(3);
    const std::uint32_t reserved = call.u32(4);
    if (reserved != 0) {
        // 文档：lpReserved 必须为 NULL
        cpu.shim.last_error = kErrInvalidParameter;
        shimLog("kernel32", api, "lpReserved 非 NULL ⇒ 返回 0 + 错误 87");
        call.result = 0;
        return;
    }
    if (!isConsoleHandle(h)) {
        // 文档：句柄不是控制台句柄 ⇒ ERROR_INVALID_HANDLE
        cpu.shim.last_error = 6u;
        shimLog("kernel32", api, "0x%08X 不是控制台句柄（只有 GetStdHandle 的三个伪句柄）⇒ 返回 0", h);
        call.result = 0;
        return;
    }
    if (nchars > 0 && buf == 0) {
        cpu.shim.last_error = kErrInvalidParameter;
        shimLog("kernel32", api, "lpBuffer 为 NULL 但长度 %u > 0 ⇒ 返回 0 + 错误 87", nchars);
        call.result = 0;
        return;
    }
    if (nchars > 0) {
        requireCommitted(cpu, buf, static_cast<std::size_t>(nchars) * 2u, api, "lpBuffer");
        std::u16string w;
        w.reserve(nchars);
        for (std::uint32_t i = 0; i < nchars; ++i) {
            w.push_back(static_cast<char16_t>(cpu.mem->rd16(buf + i * 2u)));
        }
        std::size_t bad = 0;
        const std::string utf8 = utf16ToUtf8(w, &bad);
        if (bad != 0) {
            shimLog("kernel32", api, "有 %u 个未配对代理字符（已写 '?'）", static_cast<unsigned>(bad));
        }
        // 真机写到控制台屏幕缓冲区；这里落到宿主 stdout（确定性的字节序列，不带时间戳）
        if (!utf8.empty()) std::fwrite(utf8.data(), 1, utf8.size(), stdout);
        std::fflush(stdout);
    }
    if (written_ea != 0) {
        requireCommitted(cpu, written_ea, 4, api, "lpNumberOfCharsWritten");
        cpu.mem->wr32(written_ea, nchars);
    }
    call.result = 1;
}

// 文档：VOID，把字符串送给调试器；没有调试器时什么都不做。
// 本模拟器把它送进宿主日志（一行一条；⛔ 不改变 guest 可见状态）。
void shimOutputDebugStringA(X86& cpu, ShimCall& call) {
    (void)cpu;
    const std::uint32_t ea = call.u32(0);
    if (ea == 0) {
        call.result = 0;  // 文档：lpOutputString 可选 ⇒ NULL 时什么也不做
        return;
    }
    const std::string text = cpu.mem->readCString(ea);
    shimLog("kernel32", "OutputDebugStringA", "%s", text.c_str());
    call.result = 0;
}

void shimGetSystemInfo(X86& cpu, ShimCall& call) {
    requireMem(cpu, "GetSystemInfo");
    const std::uint32_t ea = call.u32(0);
    if (ea == 0) shimAbort("kernel32", "GetSystemInfo", "lpSystemInfo 为 NULL");
    cpu.mem->fillBytes(ea, 0, kSystemInfoSize);
    cpu.mem->wr16(ea + 0x00, 0u);           // wProcessorArchitecture = x86 (INTEL)
    cpu.mem->wr16(ea + 0x02, 0u);           // wReserved
    cpu.mem->wr32(ea + 0x04, 0x1000u);      // dwPageSize
    cpu.mem->wr32(ea + 0x08, 0x00010000u);  // lpMinimumApplicationAddress
    cpu.mem->wr32(ea + 0x0C, 0x7FFEFFFFu);  // lpMaximumApplicationAddress
    // +0x10 dwActiveProcessorMask：按本批口径留 0（偏差 5）
    cpu.mem->wr32(ea + 0x14, 1u);           // dwNumberOfProcessors
    cpu.mem->wr32(ea + 0x18, 586u);         // dwProcessorType = PROCESSOR_INTEL_PENTIUM
    cpu.mem->wr32(ea + 0x1C, 0x00010000u);  // dwAllocationGranularity
    cpu.mem->wr16(ea + 0x20, 0u);           // wProcessorLevel
    cpu.mem->wr16(ea + 0x22, 0u);           // wProcessorRevision
    call.result = 0;
}

// ---- 调试 / 异常 ----
void shimIsDebuggerPresent(X86& cpu, ShimCall& call) {
    (void)cpu;
    call.result = 0;
}

void shimSetUnhandledExceptionFilter(X86& cpu, ShimCall& call) {
    (void)cpu;
    call.result = 0;  // 没有上一个过滤器
}

void shimUnhandledExceptionFilter(X86& cpu, ShimCall& call) {
    (void)cpu;
    call.result = 1;  // EXCEPTION_EXECUTE_HANDLER
}

// ---- 退出 ----
void shimExitProcess(X86& cpu, ShimCall& call) {
    cpu.shim.exit_code = call.u32(0);
    cpu.running = false;
    shimLog("kernel32", "ExitProcess", "guest 请求退出：exit_code=%u（cpu.running=false）",
            cpu.shim.exit_code);
    call.result = 0;
}

void shimTerminateProcess(X86& cpu, ShimCall& call) {
    const std::uint32_t h = call.u32(0);
    cpu.shim.exit_code = call.u32(1);
    cpu.running = false;
    // 偏差 6：只有一个进程 ⇒ 不看句柄内容
    shimLog("kernel32", "TerminateProcess", "hProcess=0x%08X：本模拟器只有一个 guest 进程 ⇒ 停下，exit_code=%u",
            h, cpu.shim.exit_code);
    call.result = 1;
}

// ---- 动态加载 / 导出 ----
// ★ 导出的真源 = **shim 注册表**（`ShimRegistry`），不是任何 PE 导出表：
//   `GetProcAddress` 把模块句柄映射回模块 key，再 `shims->find(key, name)`。
//   · 找得到且有 `stub_ea` ⇒ 返回**桩页里的那条唯一地址**（guest 的 `call eax` 于是与 `call [IAT]`
//     走同一条 dispatcher 路径）；
//   · **`fn == nullptr` 也照样返回**：这是"有这个导出、但我们还没实现"的**诚实形态** ——
//     guest 真调它会在 dispatcher 里按 reason 响亮失败（⛔ 不是在这里返回 0 假装没有这个函数）；
//   · 找不到 ⇒ 0 + `ERROR_PROC_NOT_FOUND(127)`；而"句柄根本不认得"是 `ERROR_MOD_NOT_FOUND(126)`，
//     两者日志分开（⛔ 不要都塞进一句"找不到"）。
// ★ 运行期添加的导出（DDraw 的 `DirectDrawCreate` 之类）走 `ShimRegistry::addRuntime`：
//   它在 bind() 之后继续发唯一地址，所以**不需要**改 PE / 重跑 bind。
// ★ AGERC 是**真实装载的 guest 模块**（2026-11-04 改口径）：句柄 = 真实模块基址
//   （`cpu.shim.agerc_base`），模态 no-op 只覆盖它那 3 个导出（`addStub` 的 thunk，在通用路径里
//   同样找得到）。额外保留"未 LoadLibrary 就没有句柄 / 没有镜像就装不上"这两条真机边界。
//
// 模块句柄 → 模块 key（`GetProcAddress` 用）。空串 = **不认得这个句柄**。
// ★ 判据是"这个句柄是本模拟器发出去的吗"：假句柄表 = `moduleKeyTable` 的去重序号（见上面"模块表"）；
//   主模块 / AGERC 的句柄是**真实基址**，单独认。⛔ 不用名字哈希（会撞）。
std::string moduleKeyForHandle(X86& cpu, std::uint32_t handle) {
    if (handle == 0) return {};
    if (handle == cpu.shim.module_handle) {
        const std::string exe = exePath(cpu);
        return exe.empty() ? std::string("image") : moduleKey(exe);
    }
    // ★ AGERC 的句柄是**真实模块基址**（`cpu.shim.agerc_base`），不参与假句柄编号 ⇒ 单独认。
    if (cpu.shim.agerc_base != 0 && handle == cpu.shim.agerc_base) return "agerc";
    const std::vector<std::string> table = moduleKeyTable(cpu);
    for (std::size_t i = 0; i < table.size(); ++i)
        if (moduleHandleForIndex(i) == handle) return table[i];
    return {};
}

void shimGetProcAddress(X86& cpu, ShimCall& call) {
    requireMem(cpu, "GetProcAddress");
    const std::uint32_t mod = call.u32(0);
    const std::uint32_t name_ea = call.u32(1);
    // 名字：字符串指针；`< 0x10000` = MAKEINTRESOURCE 的**序号**（bind 那侧登记成 `#N`）
    std::string name;
    if (name_ea != 0 && name_ea < 0x10000u) {
        name = "#" + std::to_string(name_ea);
    } else if (name_ea != 0) {
        name = call.str(1);
    }
    const char* shown = name.empty() ? "<null>" : name.c_str();

    // ① 句柄 → 模块 key（这一步失败就是"模块不认得"，与"模块没有这个导出"分开报）
    const std::string key = moduleKeyForHandle(cpu, mod);
    if (key.empty()) {
        cpu.shim.last_error = kErrModNotFound;
        shimLog("kernel32", "GetProcAddress",
                "模块句柄 0x%08X 不认得（不是本进程发出去的句柄）⇒ 返回 0 + 错误 126", mod);
        call.result = 0;
        return;
    }
    // ★ AGERC：句柄只能从 LoadLibrary 来（真机事实）⇒ 没装载过就没有句柄可查。
    if (key == "agerc" && !cpu.shim.agerc_loaded) {
        cpu.shim.last_error = kErrModNotFound;
        shimLog("kernel32", "GetProcAddress",
                "AGERC 没有装载过（句柄只能来自 LoadLibraryA/W）⇒ 返回 0 + 错误 126");
        call.result = 0;
        return;
    }

    // ② 查 shim 注册表（导出的**唯一**真源）
    const ShimDef* def = cpu.shims ? cpu.shims->find(key.c_str(), shown) : nullptr;
    if (def && def->stub_ea != 0) {
        if (key == "agerc") {
            shimLog("kernel32", "GetProcAddress",
                    "AGERC!%s ⇒ stub thunk 0x%08X（真实 AGERC 镜像在 0x%08X；三个导出仍按 SCOPE §1 "
                    "模态 no-op）",
                    shown, def->stub_ea, cpu.shim.agerc_base);
        } else {
            shimLog("kernel32", "GetProcAddress", "%s!%s ⇒ thunk 0x%08X（%s）", key.c_str(), shown,
                    def->stub_ea,
                    def->fn ? "已实现" : "有这个导出但还没实现：调用它会在 dispatcher 里响亮失败");
        }
        call.result = def->stub_ea;
        return;
    }

    // ③ 模块认得、但没有这个导出
    cpu.shim.last_error = kErrProcNotFound;
    if (key == "agerc") {
        shimLog("kernel32", "GetProcAddress",
                "AGERC 没有导出 \"%s\"（只有 _GetInstance@0 / _ShowDialog@12 / _OperateMenu@16）⇒ "
                "返回 0 + 错误 127",
                shown);
    } else {
        shimLog("kernel32", "GetProcAddress", "模块 \"%s\" 里没有导出 \"%s\" ⇒ 返回 0 + 错误 127",
                key.c_str(), shown);
    }
    call.result = 0;
}

// `LoadLibraryA/W`：问的是"**系统里有这个 DLL 吗**"（有就装得上），与 `GetModuleHandle` 的
// "此刻在进程里吗"不是同一个问题（两张表见文件上方）。系统 DLL 返回**确定性假句柄**；
// AGERC.DLL 返回**真实模块基址**（没有镜像 ⇒ 0 + 126，⛔ 不假装成功）。
// ⛔ **装载本身不带来任何导出**：导出由 shim 注册表决定 —— 有人用 `add` / `addStub` / `addRuntime`
//    登记过就有，没有就 127。⚠ 偏差按 SCOPE §3 的形态登记：合理值 + 打日志。
void loadLibrary(X86& cpu, ShimCall& call, bool wide) {
    const char* api = wide ? "LoadLibraryW" : "LoadLibraryA";
    const std::string name = wide ? narrowAscii(call.wstr(0)) : call.str(0);
    const std::string key = moduleKey(name);
    // ★ AGERC：唯一一个真实装载的模块。没有镜像 ⇒ 0 + 126（⛔ 没有假句柄兜底）。
    if (key == "agerc") {
        if (cpu.shim.agerc_base == 0) {
            cpu.shim.last_error = kErrModNotFound;
            shimLog("kernel32", api,
                    "\"%s\" 没有装载到 guest（没有 AGERC 镜像：--agerc / --game-dir 下的 AGERC.DLL）"
                    " ⇒ 返回 0 + 错误 126（⛔ 不假装成功）",
                    name.empty() ? "<null>" : name.c_str());
            call.result = 0;
            return;
        }
        cpu.shim.agerc_loaded = true;
        shimLog("kernel32", api,
                "装载 \"%s\" ⇒ **真实模块基址 0x%08X**（PE 节已映射进 guest；三个导出仍走 stub "
                "thunk，SCOPE §1 模态 no-op）",
                name.c_str(), cpu.shim.agerc_base);
        call.result = cpu.shim.agerc_base;
        return;
    }
    const std::uint32_t handle =
        isLoadableModule(name) ? moduleHandleIn(cpu, moduleKeyTable(cpu), name) : 0;
    if (handle) {
        shimLog("kernel32", api,
                "装载 \"%s\" ⇒ 假句柄 0x%08X（导出由 shim 注册表决定：登记过的能查到，其余 127）",
                name.c_str(), handle);
        call.result = handle;
        return;
    }
    cpu.shim.last_error = kErrModNotFound;
    shimLog("kernel32", api, "不认识的模块 \"%s\" ⇒ 返回 0 + 错误 126",
            name.empty() ? "<null>" : name.c_str());
    call.result = 0;
}

void shimLoadLibraryA(X86& cpu, ShimCall& call) { loadLibrary(cpu, call, false); }
void shimLoadLibraryW(X86& cpu, ShimCall& call) { loadLibrary(cpu, call, true); }

void shimFreeLibrary(X86& cpu, ShimCall& call) {
    (void)cpu;
    (void)call;
    call.result = 1;  // 偏差 7：没有可卸载的模块（幂等无害）
}

void shimDisableThreadLibraryCalls(X86& cpu, ShimCall& call) {
    (void)cpu;
    (void)call;
    call.result = 1;  // 偏差 7
}

}  // namespace

void registerProcessShims(ShimRegistry& shims) {
    // ---- 模块 ----
    shims.add("kernel32", "GetModuleHandleA", Conv::Stdcall, 1, shimGetModuleHandleA);
    shims.add("kernel32", "GetModuleHandleW", Conv::Stdcall, 1, shimGetModuleHandleW);
    shims.add("kernel32", "GetModuleHandleExA", Conv::Stdcall, 3, shimGetModuleHandleExA);
    shims.add("kernel32", "GetModuleHandleExW", Conv::Stdcall, 3, shimGetModuleHandleExW);
    shims.add("kernel32", "GetModuleFileNameA", Conv::Stdcall, 3, shimGetModuleFileNameA);
    shims.add("kernel32", "GetModuleFileNameW", Conv::Stdcall, 3, shimGetModuleFileNameW);

    // ---- 命令行 / 启动信息 ----
    shims.add("kernel32", "GetCommandLineA", Conv::Stdcall, 0, shimGetCommandLineA);
    shims.add("kernel32", "GetCommandLineW", Conv::Stdcall, 0, shimGetCommandLineW);
    shims.add("kernel32", "GetStartupInfoA", Conv::Stdcall, 1, shimGetStartupInfoA);
    shims.add("kernel32", "GetStartupInfoW", Conv::Stdcall, 1, shimGetStartupInfoW);

    // ---- 版本 / 系统信息 / 计算机名 ----
    shims.add("kernel32", "GetVersion", Conv::Stdcall, 0, shimGetVersion);
    shims.add("kernel32", "GetVersionExA", Conv::Stdcall, 1, shimGetVersionExA);
    shims.add("kernel32", "GetVersionExW", Conv::Stdcall, 1, shimGetVersionExW);
    shims.add("kernel32", "GetSystemInfo", Conv::Stdcall, 1, shimGetSystemInfo);
    shims.add("kernel32", "GetComputerNameA", Conv::Stdcall, 2, shimGetComputerNameA);

    // ---- 整数运算 / 控制台 / 调试输出 ----
    shims.add("kernel32", "MulDiv", Conv::Stdcall, 3, shimMulDiv);
    shims.add("kernel32", "GetConsoleCP", Conv::Stdcall, 0, shimGetConsoleCP);
    shims.add("kernel32", "WriteConsoleW", Conv::Stdcall, 5, shimWriteConsoleW);
    shims.add("kernel32", "OutputDebugStringA", Conv::Stdcall, 1, shimOutputDebugStringA);

    // ---- 调试 / 异常过滤 ----
    shims.add("kernel32", "IsDebuggerPresent", Conv::Stdcall, 0, shimIsDebuggerPresent);
    shims.add("kernel32", "SetUnhandledExceptionFilter", Conv::Stdcall, 1,
              shimSetUnhandledExceptionFilter);
    shims.add("kernel32", "UnhandledExceptionFilter", Conv::Stdcall, 1, shimUnhandledExceptionFilter);

    // ---- 退出 ----
    shims.add("kernel32", "ExitProcess", Conv::Stdcall, 1, shimExitProcess);
    shims.add("kernel32", "TerminateProcess", Conv::Stdcall, 2, shimTerminateProcess);

    // ---- 动态加载 / 导出（导出真源 = shim 注册表；见文件上方与 shimGetProcAddress）----
    shims.add("kernel32", "GetProcAddress", Conv::Stdcall, 2, shimGetProcAddress);
    shims.add("kernel32", "LoadLibraryA", Conv::Stdcall, 1, shimLoadLibraryA);
    shims.add("kernel32", "LoadLibraryW", Conv::Stdcall, 1, shimLoadLibraryW);
    shims.add("kernel32", "FreeLibrary", Conv::Stdcall, 1, shimFreeLibrary);
    shims.add("kernel32", "DisableThreadLibraryCalls", Conv::Stdcall, 1, shimDisableThreadLibraryCalls);

    // ---- 进程创建：guest 单线程 / 单进程（README §2.5）⇒ 显式缺口 ----
    shims.addUnimplemented("kernel32", "CreateProcessA",
                           "不属 M1（自替换 / 重启路径；3 处调用，执行源已是拆壳后 exe）⇒ 本线不实现，命中即 trap");
}

}  // namespace lift
