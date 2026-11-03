// emulator-lift · kernel32 进程 / 模块信息族（+ CRT 启动路径上会碰到的几条）
//
// ★ 这一族的口径：
//   · 模块句柄 = `cpu.shim.module_handle`（= image base，**不是**宿主模块句柄）；
//     exe 全路径 / 命令行在 guest 里各有一份（`cpu.shim.exe_path_ea` / `cmdline_ea`），由
//     `setupProcessStructures` 写好，这里只读它、不再拼字符串。
//   · `GetModuleHandleA/W(0)` 与"等于 exe 文件名（大小写不敏感、可省 `.exe`）"都返回主模块句柄；
//     其余按**"此刻在进程里吗"**回答（见下面"模块表"一节）：`ntdll` / `kernel32` + 本镜像**导入表**里
//     的模块 ⇒ 非 0 假句柄；其余 ⇒ 0 + `SetLastError(126)` + 一行日志。
//   · `GetProcAddress` **不提供导出转发**（本模拟器不导出任何函数），恒 127 + 日志；
//     `LoadLibraryA/W` 只回答"系统里有没有这个 DLL"（有就给假句柄），仍然不带来任何导出。
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
//   8. **文件 / 路径 / 注册表族不在本批**：`GetSystemDirectoryA` / `GetWindowsDirectoryA` /
//      `GetTempPathA` / `GetCurrentDirectoryA` / `SetCurrentDirectoryA` / `GetModuleHandleExA`
//      用 `addUnimplemented` 登记（S6 文件与路径族不在 M1a 这一批），执行到就响亮失败。
//   9. **模块句柄是确定性假值**（`0x7F000000 + 表内序号*64KiB`）：本模拟器不真的装载 DLL，
//      但 `ntdll` / `kernel32` / 导入表里的模块在真机上**确实在进程里** ⇒ 返回 0 是假失败
//      （实测：CRT 的 `__scrt_initialize_fls` 因此返回 false ⇒ `_fast_error_exit` ⇒ 到不了 WinMain）。
//      影响：句柄只能当**不透明令牌**用（比较 / 传给 `GetProcAddress`）；⛔ 不要拿它去读 PE 头。
//      导出表仍然是空的（`GetProcAddress` 一律 127），所以"模块存在"不等于"函数可用"。
#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

#include "runtime/sha256.h"
#include "runtime/x86.h"
#include "shims/register.h"

namespace lift {
namespace {

constexpr std::uint32_t kErrInvalidParameter = 87u;
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

// 把 ANSI 串写进 guest 缓冲：截断到 cap-1 并保证 0 结尾，返回**写入的字节数**（不含结尾 0）
std::uint32_t writeAnsi(X86& cpu, std::uint32_t ea, std::uint32_t cap, const std::string& s) {
    const std::size_t n = std::min<std::size_t>(s.size(), static_cast<std::size_t>(cap) - 1);
    if (n > 0) cpu.mem->writeBytes(ea, s.data(), n);
    cpu.mem->wr8(ea + static_cast<std::uint32_t>(n), 0);
    return static_cast<std::uint32_t>(n);
}

// 把 ANSI 串按"字节 → 16 位"扩展写进 guest 缓冲（偏差 2）
std::uint32_t writeWide(X86& cpu, std::uint32_t ea, std::uint32_t cap_chars, const std::string& s) {
    const std::size_t n =
        std::min<std::size_t>(s.size(), static_cast<std::size_t>(cap_chars) - 1);
    for (std::size_t i = 0; i < n; ++i) {
        const auto ch = static_cast<std::uint16_t>(static_cast<unsigned char>(s[i]));
        cpu.mem->wr16(ea + static_cast<std::uint32_t>(i) * 2u, ch);
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
// ⛔ 导出仍然**一律不存在**：GetProcAddress 恒 127（本模拟器不导出任何函数）——
//    "模块存在、函数不存在"是 M1a 的真实边界（plan §6 S6 后四族）。
constexpr std::uint32_t kFakeModuleBase = 0x7F000000u;
constexpr std::uint32_t kFakeModuleStride = 0x10000u;

// `LoadLibraryA/W` 的"系统里存在"表：真机上这些 DLL 都能装载成功。
const char* const kLoadableModules[] = {
    "ntdll.dll",    "kernel32.dll", "user32.dll",   "gdi32.dll",     "advapi32.dll",
    "winmm.dll",    "ole32.dll",    "oleaut32.dll", "shell32.dll",   "comctl32.dll",
    "version.dll",  "imm32.dll",    "ddraw.dll",    "d3d9.dll",      "d3dx9_43.dll",
    "ws2_32.dll",   "imagehlp.dll", "dxva2.dll",    "wininet.dll",   "mscoree.dll",
    "shlwapi.dll",  "mf.dll",       "mfplat.dll",   "evr.dll",
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

// 在表里的序号 → 句柄；0 = 不在表里
std::uint32_t moduleHandleIn(const std::vector<std::string>& table, const std::string& name) {
    const std::string want = moduleKey(name);
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
    if (!cpu.shims) return false;
    for (const ShimDef& d : cpu.shims->all())
        if (d.imported && moduleKey(d.module) == key) return true;
    return false;
}

// ---- 模块 ----
void getModuleHandle(X86& cpu, ShimCall& call, bool wide) {
    const char* api = wide ? "GetModuleHandleW" : "GetModuleHandleA";
    requireMem(cpu, api);
    const std::uint32_t name_ea = call.u32(0);
    if (name_ea == 0) {
        call.result = cpu.shim.module_handle;
        return;
    }
    const std::string want = wide ? narrowAscii(call.wstr(0)) : call.str(0);
    const std::string exe = exePath(cpu);
    if (!moduleNameMatches(want, exe)) {
        // ★ 判据是"**已装载**"（导入表里的 + kernel32/ntdll），⛔ 不是"系统里有"：
        //   例 `mscoree.dll` 系统里有、但本进程没装载 ⇒ 真机返回 0，这里也必须返回 0。
        if (moduleIsLoaded(cpu, moduleKey(want))) {
            const std::uint32_t handle = moduleHandleIn(moduleKeyTable(cpu), want);
            shimLog("kernel32", api,
                    "\"%s\" 已装载 ⇒ 假句柄 0x%08X（导出仍不提供，GetProcAddress 会 127）", want.c_str(),
                    handle);
            call.result = handle;
            return;
        }
        cpu.shim.last_error = kErrModNotFound;
        shimLog("kernel32", api, "主模块是 \"%s\"；\"%s\" 未装载 ⇒ 返回 0 + 错误 126",
                exe.empty() ? "<未设置>" : exe.c_str(), want.c_str());
        call.result = 0;
        return;
    }
    call.result = cpu.shim.module_handle;
}

void shimGetModuleHandleA(X86& cpu, ShimCall& call) { getModuleHandle(cpu, call, false); }
void shimGetModuleHandleW(X86& cpu, ShimCall& call) { getModuleHandle(cpu, call, true); }

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
    const std::string path = exePath(cpu);
    if (path.empty()) {
        cpu.shim.last_error = kErrInvalidParameter;
        shimLog("kernel32", api, "guest 里没有 exe 全路径（exe_path_ea == 0）⇒ 返回 0");
        call.result = 0;
        return;
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

void shimGetVersionExA(X86& cpu, ShimCall& call) {
    requireMem(cpu, "GetVersionExA");
    const std::uint32_t ea = call.u32(0);
    if (ea == 0) shimAbort("kernel32", "GetVersionExA", "lpVersionInformation 为 NULL");
    const std::uint32_t size = cpu.mem->rd32(ea);  // 保持调用方给的值（不覆盖）
    if (size < 0x14) {
        cpu.shim.last_error = kErrInsufficientBuffer;
        shimLog("kernel32", "GetVersionExA", "dwOSVersionInfoSize=%u 太小（< 0x14）⇒ 返回 0", size);
        call.result = 0;
        return;
    }
    cpu.mem->wr32(ea + 0x04, 5u);      // dwMajorVersion
    cpu.mem->wr32(ea + 0x08, 1u);      // dwMinorVersion
    cpu.mem->wr32(ea + 0x0C, 2600u);   // dwBuildNumber
    cpu.mem->wr32(ea + 0x10, kWin32Nt);  // dwPlatformId
    // szCSDVersion[128]：清零（最多到调用方给的长度）
    const std::uint32_t csd_bytes = std::min<std::uint32_t>(size - 0x14, 0x80u);
    cpu.mem->fillBytes(ea + 0x14, 0, csd_bytes);
    call.result = 1;
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

// ---- 动态加载 / 导出（本模拟器不提供）----
void shimGetProcAddress(X86& cpu, ShimCall& call) {
    requireMem(cpu, "GetProcAddress");
    const std::uint32_t mod = call.u32(0);
    const std::uint32_t name_ea = call.u32(1);
    std::string name;
    if (name_ea != 0 && name_ea < 0x10000u) {
        name = "<序号 " + std::to_string(name_ea) + ">";
    } else if (name_ea != 0) {
        name = call.str(1);
    }
    cpu.shim.last_error = kErrProcNotFound;
    shimLog("kernel32", "GetProcAddress",
            "模块 0x%08X 里找不到 \"%s\"：本模拟器不提供导出转发（导入走 IAT 桩页，见 shims.h）⇒ 返回 0",
            mod, name.empty() ? "<null>" : name.c_str());
    call.result = 0;
}

// `LoadLibraryA/W`：问的是"**系统里有这个 DLL 吗**"（有就装得上），与 `GetModuleHandle` 的
// "此刻在进程里吗"不是同一个问题（两张表见文件上方）。返回**确定性假句柄**。
// ⛔ 装载成功仍然**不带来任何导出**：`GetProcAddress` 对它一律 127 —— "模块存在、函数不存在"
//    是 M1a 的真实边界（plan §6 S6 后四族）。⚠ 偏差按 SCOPE §3 的形态登记：合理值 + 打日志。
void loadLibrary(X86& cpu, ShimCall& call, bool wide) {
    const char* api = wide ? "LoadLibraryW" : "LoadLibraryA";
    const std::string name = wide ? narrowAscii(call.wstr(0)) : call.str(0);
    const std::uint32_t handle = isLoadableModule(name) ? moduleHandleIn(moduleKeyTable(cpu), name) : 0;
    if (handle) {
        shimLog("kernel32", api, "装载 \"%s\" ⇒ 假句柄 0x%08X（本模拟器不导出任何函数，GetProcAddress 仍会 127）",
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
    shims.add("kernel32", "GetModuleFileNameA", Conv::Stdcall, 3, shimGetModuleFileNameA);
    shims.add("kernel32", "GetModuleFileNameW", Conv::Stdcall, 3, shimGetModuleFileNameW);

    // ---- 命令行 / 启动信息 ----
    shims.add("kernel32", "GetCommandLineA", Conv::Stdcall, 0, shimGetCommandLineA);
    shims.add("kernel32", "GetCommandLineW", Conv::Stdcall, 0, shimGetCommandLineW);
    shims.add("kernel32", "GetStartupInfoA", Conv::Stdcall, 1, shimGetStartupInfoA);
    shims.add("kernel32", "GetStartupInfoW", Conv::Stdcall, 1, shimGetStartupInfoW);

    // ---- 版本 / 系统信息 ----
    shims.add("kernel32", "GetVersion", Conv::Stdcall, 0, shimGetVersion);
    shims.add("kernel32", "GetVersionExA", Conv::Stdcall, 1, shimGetVersionExA);
    shims.add("kernel32", "GetSystemInfo", Conv::Stdcall, 1, shimGetSystemInfo);

    // ---- 调试 / 异常过滤 ----
    shims.add("kernel32", "IsDebuggerPresent", Conv::Stdcall, 0, shimIsDebuggerPresent);
    shims.add("kernel32", "SetUnhandledExceptionFilter", Conv::Stdcall, 1,
              shimSetUnhandledExceptionFilter);
    shims.add("kernel32", "UnhandledExceptionFilter", Conv::Stdcall, 1, shimUnhandledExceptionFilter);

    // ---- 退出 ----
    shims.add("kernel32", "ExitProcess", Conv::Stdcall, 1, shimExitProcess);
    shims.add("kernel32", "TerminateProcess", Conv::Stdcall, 2, shimTerminateProcess);

    // ---- 动态加载 / 导出（本模拟器不提供；失败值 + 日志）----
    shims.add("kernel32", "GetProcAddress", Conv::Stdcall, 2, shimGetProcAddress);
    shims.add("kernel32", "LoadLibraryA", Conv::Stdcall, 1, shimLoadLibraryA);
    shims.add("kernel32", "LoadLibraryW", Conv::Stdcall, 1, shimLoadLibraryW);
    shims.add("kernel32", "FreeLibrary", Conv::Stdcall, 1, shimFreeLibrary);
    shims.add("kernel32", "DisableThreadLibraryCalls", Conv::Stdcall, 1, shimDisableThreadLibraryCalls);

    // ---- 文件 / 路径 / 模块枚举：S6 文件与路径族不在 M1a 这一批 ⇒ 登记为未实现 ----
    shims.addUnimplemented("kernel32", "GetSystemDirectoryA", "S6 文件与路径族不在 M1a 这一批");
    shims.addUnimplemented("kernel32", "GetWindowsDirectoryA", "S6 文件与路径族不在 M1a 这一批");
    shims.addUnimplemented("kernel32", "GetTempPathA", "S6 文件与路径族不在 M1a 这一批");
    shims.addUnimplemented("kernel32", "GetCurrentDirectoryA", "S6 文件与路径族不在 M1a 这一批");
    shims.addUnimplemented("kernel32", "SetCurrentDirectoryA", "S6 文件与路径族不在 M1a 这一批");
    shims.addUnimplemented("kernel32", "GetModuleHandleExA", "S6 文件与路径族不在 M1a 这一批");
    shims.addUnimplemented("kernel32", "GetModuleHandleExW", "S6 文件与路径族不在 M1a 这一批");
}

}  // namespace lift
