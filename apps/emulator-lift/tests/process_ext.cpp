// emulator-lift · 进程 / 模块信息族的本轮新增（pure：自造调用序列，不碰游戏 / LFS）
//
// 判据（plan §5 A5 / §4 D6 "进程·模块信息族"）：
//   ① `GetComputerNameA`：固定名、*nSize 的成功/失败两种口径（失败 ⇒ ERROR_BUFFER_OVERFLOW）；
//   ② `GetVersionExW`：与 A 版**同源**（同一份 impl）、`dwOSVersionInfoSize` 太小 ⇒ 0 + 122、
//      OSVERSIONINFOEX 的扩展字段被清 0；
//   ③ `GetModuleHandleExA/W`：按名字（已装载 / 未装载）、按地址（FROM_ADDRESS）、
//      PIN 与 UNCHANGED_REFCOUNT 互斥、失败时 *phModule = NULL；
//   ④ `MulDiv`：四舍五入（正半向上 / 负半向下 = 远离零）、分母 0 / 溢出 ⇒ -1；
//   ⑤ `GetConsoleCP` / `WriteConsoleW`（参数校验 + 写出的字符数）/ `OutputDebugStringA`；
//   ⑥ `VirtualLock`：已提交 ⇒ TRUE；未提交 ⇒ FALSE + ERROR_INVALID_ADDRESS。
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "check.h"
#include "fixture.h"
#include "runtime/machine.h"
#include "shims/register.h"

namespace {

using lift_fixture::Asm;
using lift_fixture::Fixture;

constexpr std::uint32_t kErrInvalidHandle = 6u;
constexpr std::uint32_t kErrInvalidParameter = 87u;
constexpr std::uint32_t kErrBufferOverflow = 111u;
constexpr std::uint32_t kErrInsufficientBuffer = 122u;
constexpr std::uint32_t kErrModNotFound = 126u;
constexpr std::uint32_t kErrInvalidAddress = 487u;

constexpr std::uint32_t kGetModuleHandleExFromAddress = 0x00000004u;
constexpr std::uint32_t kGetModuleHandleExPin = 0x00000001u;
constexpr std::uint32_t kGetModuleHandleExUnchanged = 0x00000002u;

std::string guestStr(lift::Machine& m, std::uint32_t ea, std::uint32_t cap = 512) {
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

    // ① GetComputerNameA
    {
        g_current_case = "get-computer-name";
        Fixture f;
        lift::registerProcessShims(f.m.shims());
        LIFT_CHECK(f.ok);
        const std::uint32_t gcn = f.use("GetComputerNameA");
        const std::uint32_t buf = f.zeroData(64);
        const std::uint32_t size = f.u32data(64u);
        LIFT_CHECK_EQ_U32(f.call(gcn, {buf, size}), 1u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(size), 8u);  // "AGE-LIFT" 8 个字符（不含结尾 0）
        LIFT_CHECK(guestStr(f.m, buf) == "AGE-LIFT");
        // 缓冲太小 ⇒ FALSE + 111，*nSize = 所需大小（含结尾 0）
        const std::uint32_t small = f.u32data(4u);
        LIFT_CHECK_EQ_U32(f.call(gcn, {buf, small}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrBufferOverflow);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(small), 9u);
        // NULL 参数 ⇒ 87
        LIFT_CHECK_EQ_U32(f.call(gcn, {0u, small}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrInvalidParameter);
        LIFT_CHECK(f.ok);
    }

    // ② GetVersionExW（与 A 版同源）
    {
        g_current_case = "get-version-ex-w";
        Fixture f;
        lift::registerProcessShims(f.m.shims());
        LIFT_CHECK(f.ok);
        const std::uint32_t gve = f.use("GetVersionExW");
        const std::uint32_t gve_a = f.use("GetVersionExA");

        // OSVERSIONINFOW（0x14 字节）：先写好 dwOSVersionInfoSize
        const std::uint32_t info = f.zeroData(0x14);
        f.m.mem().wr32(info, 0x14u);
        LIFT_CHECK_EQ_U32(f.call(gve, {info}), 1u);
        LIFT_CHECK_MEM_U32(f.m.mem(), info, 0x14u);   // 调用方给的值不被覆盖
        LIFT_CHECK_MEM_U32(f.m.mem(), info + 0x04, 5u);
        LIFT_CHECK_MEM_U32(f.m.mem(), info + 0x08, 1u);
        LIFT_CHECK_MEM_U32(f.m.mem(), info + 0x0C, 2600u);
        LIFT_CHECK_MEM_U32(f.m.mem(), info + 0x10, 2u);
        // 与 A 版逐字段相同（同源）
        const std::uint32_t info_a = f.zeroData(0x14);
        f.m.mem().wr32(info_a, 0x14u);
        LIFT_CHECK_EQ_U32(f.call(gve_a, {info_a}), 1u);
        for (std::uint32_t off = 0; off < 0x14u; off += 4u) {
            LIFT_CHECK_EQ_U32(f.m.mem().rd32(info_a + off), f.m.mem().rd32(info + off));
        }

        // size 太小 ⇒ 0 + ERROR_INSUFFICIENT_BUFFER
        const std::uint32_t bad = f.zeroData(0x10);
        f.m.mem().wr32(bad, 0x10u);
        LIFT_CHECK_EQ_U32(f.call(gve, {bad}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrInsufficientBuffer);

        // OSVERSIONINFOEXW（0x11C）：扩展字段必须被清 0（不能留调用方的残值）
        const std::uint32_t ex = f.zeroData(0x11C);
        f.m.mem().wr32(ex, 0x11Cu);
        f.m.mem().wr32(ex + 0x94, 0xDEADBEEFu);  // wServicePackMajor 起
        LIFT_CHECK_EQ_U32(f.call(gve, {ex}), 1u);
        LIFT_CHECK_MEM_U32(f.m.mem(), ex + 0x94, 0u);
        LIFT_CHECK_MEM_U32(f.m.mem(), ex + 0x118, 0u);
        LIFT_CHECK(f.ok);
    }

    // ③ GetModuleHandleExA / W
    {
        g_current_case = "get-module-handle-ex";
        Fixture f;
        lift::registerProcessShims(f.m.shims());
        LIFT_CHECK(f.ok);
        // 纯档夹具没有载入 PE ⇒ 手动给一个"主模块"（image base + SizeOfImage）
        f.m.cpu().shim.module_handle = 0x00400000u;
        f.m.cpu().shim.module_size = 0x00200000u;

        const std::uint32_t gmhx = f.use("GetModuleHandleExA");
        const std::uint32_t gmhxw = f.use("GetModuleHandleExW");
        const std::uint32_t out = f.u32data(0xDEADBEEFu);

        // flags=0, name=NULL ⇒ 主模块
        LIFT_CHECK_EQ_U32(f.call(gmhx, {0u, 0u, out}), 1u);
        LIFT_CHECK_MEM_U32(f.m.mem(), out, 0x00400000u);

        // "kernel32" 已装载（每个 Win32 进程都有）⇒ 非 0 假句柄
        const std::uint32_t k32 = f.cstr("kernel32");
        LIFT_CHECK_EQ_U32(f.call(gmhx, {kGetModuleHandleExUnchanged, k32, out}), 1u);
        const std::uint32_t k32_handle = f.m.mem().rd32(out);
        LIFT_CHECK_NE_U32(k32_handle, 0u);
        LIFT_CHECK_NE_U32(k32_handle, 0x00400000u);
        // "mscoree.dll" 系统里有、但**本进程没装载** ⇒ 0 + 126，*phModule = NULL
        const std::uint32_t ms = f.cstr("mscoree.dll");
        f.m.mem().wr32(out, 0xDEADBEEFu);
        LIFT_CHECK_EQ_U32(f.call(gmhx, {kGetModuleHandleExUnchanged, ms, out}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrModNotFound);
        LIFT_CHECK_MEM_U32(f.m.mem(), out, 0u);

        // FROM_ADDRESS：主模块范围内 ⇒ 主模块；范围外 ⇒ 0 + 126
        LIFT_CHECK_EQ_U32(f.call(gmhx, {kGetModuleHandleExFromAddress, 0x00401000u, out}), 1u);
        LIFT_CHECK_MEM_U32(f.m.mem(), out, 0x00400000u);
        LIFT_CHECK_EQ_U32(f.call(gmhx, {kGetModuleHandleExFromAddress, 0x70000000u, out}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrModNotFound);

        // PIN 与 UNCHANGED_REFCOUNT 互斥 ⇒ 0 + 87
        LIFT_CHECK_EQ_U32(f.call(gmhx, {kGetModuleHandleExPin | kGetModuleHandleExUnchanged, 0u, out}),
                          0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrInvalidParameter);
        // phModule 为 NULL ⇒ 0 + 87
        LIFT_CHECK_EQ_U32(f.call(gmhx, {0u, 0u, 0u}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrInvalidParameter);

        // W 版：宽字符名字 "kernel32"
        const char16_t wname[] = u"kernel32";
        const std::uint32_t wc = f.wideCstr(wname, 8u + 1u);
        LIFT_CHECK_EQ_U32(f.call(gmhxw, {kGetModuleHandleExUnchanged, wc, out}), 1u);
        LIFT_CHECK_MEM_U32(f.m.mem(), out, k32_handle);  // 与 A 版同一句柄
        LIFT_CHECK(f.ok);
    }

    // ④ MulDiv
    {
        g_current_case = "mul-div";
        Fixture f;
        lift::registerProcessShims(f.m.shims());
        LIFT_CHECK(f.ok);
        const std::uint32_t muldiv = f.use("MulDiv");
        auto md = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c) {
            return f.call(muldiv, {a, b, c});
        };
        LIFT_CHECK_EQ_U32(md(10u, 10u, 3u), 33u);   // 33.33 → 33
        LIFT_CHECK_EQ_U32(md(10u, 10u, 4u), 25u);   // 25 精确
        LIFT_CHECK_EQ_U32(md(7u, 3u, 2u), 11u);     // 10.5 → 向上
        LIFT_CHECK_EQ_U32(md(1u, 1u, 2u), 1u);      // 0.5 → 向上
        LIFT_CHECK_EQ_U32(md(0xFFFFFFFFu, 1u, 2u), 0xFFFFFFFFu);  // -1*1/2 = -0.5 → 向下 = -1
        LIFT_CHECK_EQ_U32(md(0xFFFFFFF9u, 3u, 2u), 0xFFFFFFF5u);  // -7*3/2 = -10.5 → -11
        LIFT_CHECK_EQ_U32(md(5u, 5u, 0u), 0xFFFFFFFFu);           // 分母 0 ⇒ -1
        LIFT_CHECK_EQ_U32(md(0x7FFFFFFFu, 2u, 1u), 0xFFFFFFFFu);  // 溢出 ⇒ -1
        LIFT_CHECK(f.ok);
    }

    // ⑤ 控制台 / 调试输出
    {
        g_current_case = "console-and-debug-output";
        Fixture f;
        lift::registerProcessShims(f.m.shims());
        LIFT_CHECK(f.ok);
        const std::uint32_t cp = f.use("GetConsoleCP");
        const std::uint32_t wcw = f.use("WriteConsoleW");
        const std::uint32_t ods = f.use("OutputDebugStringA");

        LIFT_CHECK_EQ_U32(f.call(cp, {}), 932u);

        const char16_t text[] = u"OK";
        const std::uint32_t wbuf = f.wideCstr(text, 2u);
        const std::uint32_t written = f.u32data(0xDEADBEEFu);
        LIFT_CHECK_EQ_U32(f.call(wcw, {0x00000010u, wbuf, 2u, written, 0u}), 1u);
        LIFT_CHECK_MEM_U32(f.m.mem(), written, 2u);
        // 非控制台句柄 ⇒ 0 + ERROR_INVALID_HANDLE
        LIFT_CHECK_EQ_U32(f.call(wcw, {0x00001234u, wbuf, 2u, written, 0u}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrInvalidHandle);
        // lpReserved 非 NULL ⇒ 0 + 87
        LIFT_CHECK_EQ_U32(f.call(wcw, {0x00000010u, wbuf, 2u, written, 1u}), 0u);
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrInvalidParameter);

        const std::uint32_t msg = f.cstr("hello from guest");
        LIFT_CHECK_EQ_U32(f.call(ods, {msg}), 0u);  // VOID
        LIFT_CHECK_EQ_U32(f.call(ods, {0u}), 0u);   // NULL ⇒ 什么也不做
        LIFT_CHECK(f.ok);
    }

    // ⑥ VirtualLock
    {
        g_current_case = "virtual-lock";
        Fixture f;
        lift::registerMemShims(f.m.shims());
        LIFT_CHECK(f.ok);
        const std::uint32_t lock = f.use("VirtualLock");
        const std::uint32_t committed = f.zeroData(64);
        LIFT_CHECK_EQ_U32(f.call(lock, {committed, 64u}), 1u);
        LIFT_CHECK_EQ_U32(f.call(lock, {0x60000000u, 16u}), 0u);  // 未提交
        LIFT_CHECK_EQ_U32(f.m.cpu().shim.last_error, kErrInvalidAddress);
        LIFT_CHECK(f.ok);
    }

    return finish("process-ext");
}
