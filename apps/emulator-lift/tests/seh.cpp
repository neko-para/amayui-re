// emulator-lift · SEH / C++ EH 派发的 **pure 守卫**（`RaiseException` / `RtlUnwind`）
//
// 真源：需求单 `REQ-01M708DF24KY0QZNEHY66S0HYW`（取证 + 设计 `.tmp/reveq/<该单>/design.md`、
// 补字节 `.tmp/reveq/01M713MSM6WKDDTMHCXR6FJX5Q/report.md`）· 实现 `src/runtime/seh.{h,cpp}` ·
// 接线 `Machine::pumpSeh`。★ 这些用例**不需要**游戏 / LFS / GPU：`initBare()` 自造字节。
//
// 四条用例与"红了意味着什么"：
//   A 单 record + 一个 guest handler
//      A1 handler 只被调 1 次（没被调 / 被调多次都红）
//      A2 `pRN` 就是链上的那条 record（把 `fs:[0]` 认错时红）
//      A3 `code/flags/nargs/Info[0]` **逐位**等于 guest 压进去的值（宿主改写/合成时红；
//         guest 自己在 `0x4F00F8/0x4F0100/0x4F0109` 校验这三个字段 ⇒ 改了 catch 会静默不成立）
//      A4 `ExceptionAddress` = `call RaiseException` 的下一条（口径错时红）
//      A5 初始 CONTEXT 的 Eip/Esp = **抛出瞬间**（Eip=调用下一条、Esp=进入 shim 时的 esp）；
//         改成"派发中途的 esp"就红
//      A6 disposition=1 真的整份装载 CONTEXT（真跳到 catch 标记）
//      A7 catch 之前 `fs:[0]` **没变**（宿主提前清链头 ⇒ 真机 catch 体 0x4138C8 会写出错的值）
//      A8 同帧 catch **不依赖** RtlUnwind（本用例一次也没调它）
//   B 两层 record 都返 0 ⇒ **必须非 ok 停机**（`UnhandledException`）
//      ★ 这是"`RaiseException` 不许变静默 no-op"的机器判据（旧形态 = 静默继续，进程 exit 0）
//   C `RtlUnwind`：`ExceptionFlags |= 2` · `fs:[0] == TargetFrame`（不含端点）·
//      TargetIp==0 正常返回（`esp == call 前 + 0x10`）· TargetIp!=0 停在目标且同一 esp 口径
//   D 影子栈：展开落地按 esp 丢"被跳过的帧"（`WindowProfiler` 直接断言；⛔ 不许静默偏大）
//
// ⛔ 不要拿这些当守卫："跑一遍 room-exit.graph 没崩"（那是 assets/真机验收）·
//    "handler 的 EA == 0x413551"（游戏数据常量，换导出就废）· 真实 CRT 的 CONTEXT 布局
//    （夹具里的 handler 是我们自己写的，它读的偏移就是我们写的偏移 ⇒ 断言恒真）。
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "fixture.h"
#include "runtime/machine.h"
#include "runtime/memory.h"
#include "runtime/window_profile.h"
#include "shims/register.h"

namespace {

using lift_fixture::Asm;
using lift_fixture::Fixture;
using lift_fixture::kSentinel;

// `log_ea` 的槽位（A / B 共用；handler 把宿主要断言的东西抄进来）
enum : std::uint32_t {
    kLogCalled = 0x00,   // handler 被调次数
    kLogExcRec = 0x04,   // pExcRec
    kLogRN = 0x08,       // pRN（第 2 个参数）
    kLogCtx = 0x0C,      // pCtx
    kLogDC = 0x10,       // pDC
    kLogCode = 0x14,     // pExcRec->ExceptionCode
    kLogFlags = 0x18,    // pExcRec->ExceptionFlags
    kLogNargs = 0x1C,    // pExcRec->NumberParameters
    kLogInfo0 = 0x20,    // pExcRec->ExceptionInformation[0]
    kLogAddr = 0x24,     // pExcRec->ExceptionAddress
    kLogCtxEip = 0x28,   // pCtx->Eip（handler 进来时）
    kLogCtxEsp = 0x2C,   // pCtx->Esp（handler 进来时）
    kLogCtxEbp = 0x30,   // pCtx->Ebp（handler 进来时）
    kLogFs0 = 0x34,      // handler 里读到的 fs:[0]
    kLogOrder0 = 0x38,   // B：第 1 次调用的 pRN
    kLogOrder1 = 0x3C,   // B：第 2 次调用的 pRN
    kLogOuter = 0x40,    // C：外层 handler 的调用次数
    kLogRetMarker = 0x44,  // C：RtlUnwind 正常返回的续点标记
    kLogBytes = 0x80,
};

constexpr std::uint32_t kCatchMarker = 0x000CA057u;
constexpr std::uint32_t kBadReturn = 0x000A11CEu;
constexpr std::uint32_t kThrowCode = 0xE06D7363u;  // 取证：_CxxThrowException 的 code
constexpr std::uint32_t kThrowFlags = 0x1u;        // 取证：EXCEPTION_NONCONTINUABLE
constexpr std::uint32_t kInfo0 = 0x19930520u;      // 取证：Info[0]（EH magic，**不许**被宿主合成）
constexpr std::uint32_t kThrowInfo = 0x00544774u;  // 取证：pThrowInfo（_TI1?AVCommand_Exit_Exception@@）
constexpr std::uint32_t kObjectPtr = 0x0022FF10u;  // 夹具的"异常对象"指针（只断言透传）

// ---- 最小指令发射器（只收本文件用到的形态；位移/立即数一律显式给）----
void movEaxEspDisp8(Asm& a, std::uint8_t d) {
    a.u8(0x8B);
    a.u8(0x44);
    a.u8(0x24);
    a.u8(d);
}
void movMemEax(Asm& a, std::uint32_t ea) {
    a.u8(0xA3);
    a.u32(ea);
}
void movMemEcx(Asm& a, std::uint32_t ea) {
    a.u8(0x89);
    a.u8(0x0D);
    a.u32(ea);
}
void movMemEsp(Asm& a, std::uint32_t ea) {
    a.u8(0x89);
    a.u8(0x25);
    a.u32(ea);
}
void movEcxEaxDisp(Asm& a, std::uint32_t d) {
    a.u8(0x8B);
    a.u8(0x88);
    a.u32(d);
}
void movEaxDispImm(Asm& a, std::uint32_t d, std::uint32_t v) {
    a.u8(0xC7);
    a.u8(0x80);
    a.u32(d);
    a.u32(v);
}
void movEaxFs0(Asm& a) {
    a.u8(0x64);
    a.u8(0xA1);
    a.u32(0);
}

std::uint32_t nextCode(Fixture& fx) {
    const std::uint32_t ea = fx.code_cursor;
    fx.code_cursor += 0x100u;
    return ea;
}

std::uint32_t place(Fixture& fx, Asm& a) {
    if (!fx.m.placeCode(a.base, a.b.data(), a.b.size(), &fx.err)) {
        fx.ok = false;
        return 0;
    }
    return a.base;
}

// `initBare()` 不建 TEB（只有 `prepare()` 建）⇒ 夹具自己铺一页、装 fs_base、写链头。
bool initTeb(Fixture& fx, std::uint32_t chain_head) {
    const std::uint32_t teb = fx.m.layout().teb_ea;
    if (!fx.m.mem().commitZeroed(teb, 0x1000, lift::RegionKind::Teb, "fixture-teb")) {
        fx.ok = false;
        fx.err = "夹具 TEB 提交失败";
        return false;
    }
    fx.m.cpu().fs_base = teb;
    fx.m.mem().wr32(teb, chain_head);
    return true;
}

// 一次 throw 的 driver：压 4 个实参 → `call RaiseException` → （正常返回才会走到的）坏标记。
// 返回 driver 的入口 EA；`ret_ea_out` = `call` 的下一条（= 期望的 ExceptionAddress / 初始 CONTEXT.Eip）。
std::uint32_t emitRaiseDriver(Fixture& fx, std::uint32_t stub_ea, std::uint32_t args_ea,
                              std::uint32_t after_ea, std::uint32_t* ret_ea_out) {
    Asm a(nextCode(fx));
    a.pushImm(args_ea);    // 第 4 参 lpArguments
    a.pushImm(3);          // 第 3 参 nNumberOfArguments
    a.pushImm(kThrowFlags);  // 第 2 参 dwExceptionFlags
    a.pushImm(kThrowCode);   // 第 1 参 dwExceptionCode
    a.callRel(stub_ea);
    if (ret_ea_out) *ret_ea_out = a.currentEa();
    a.movMemImm(after_ea, kBadReturn);
    a.ret();
    const std::uint32_t entry = place(fx, a);
    return entry;
}

// handler A：把"宿主该给对的每一样"抄进 log，然后装 CONTEXT（Eip/Esp/Ebp）并返回 1。
std::uint32_t emitHandlerA(Fixture& fx, std::uint32_t log_ea, std::uint32_t catch_ea,
                           std::uint32_t new_esp) {
    Asm a(nextCode(fx));
    // 4 个指针（stdcall：进入 handler 时 [esp] 是返回地址）
    for (std::uint8_t i = 0; i < 4; ++i) {
        movEaxEspDisp8(a, static_cast<std::uint8_t>(4u + 4u * i));
        movMemEax(a, log_ea + kLogExcRec + 4u * i);
    }
    // record 字段（pExcRec 在 [esp+4]）
    movEaxEspDisp8(a, 4);
    movEcxEaxDisp(a, 0x00);
    movMemEcx(a, log_ea + kLogCode);
    movEcxEaxDisp(a, 0x04);
    movMemEcx(a, log_ea + kLogFlags);
    movEcxEaxDisp(a, 0x10);
    movMemEcx(a, log_ea + kLogNargs);
    movEcxEaxDisp(a, 0x14);
    movMemEcx(a, log_ea + kLogInfo0);
    movEcxEaxDisp(a, 0x0C);
    movMemEcx(a, log_ea + kLogAddr);
    // CONTEXT 里宿主该填的三个（pCtx 在 [esp+0xC]）
    movEaxEspDisp8(a, 0x0C);
    movEcxEaxDisp(a, 0xB8);  // Eip
    movMemEcx(a, log_ea + kLogCtxEip);
    movEcxEaxDisp(a, 0xC4);  // Esp
    movMemEcx(a, log_ea + kLogCtxEsp);
    movEcxEaxDisp(a, 0xB4);  // Ebp
    movMemEcx(a, log_ea + kLogCtxEbp);
    // catch 之前读 fs:[0]（宿主不许提前清链头）
    movEaxFs0(a);
    movMemEax(a, log_ea + kLogFs0);
    // 装 CONTEXT：Eip = catch、Esp = 新栈、Ebp = 新栈 + 4
    movEaxEspDisp8(a, 0x0C);
    movEaxDispImm(a, 0xB8, catch_ea);
    movEaxDispImm(a, 0xC4, new_esp);
    movEaxDispImm(a, 0xB4, new_esp + 4u);
    a.incDword(log_ea + kLogCalled);
    a.movEaxImm(1);  // ExceptionContinueExecution
    a.retImm16(16);
    return place(fx, a);
}

// handler B：只记 pRN + 计数，然后返 0（ExceptionContinueSearch）。
std::uint32_t emitHandlerB(Fixture& fx, std::uint32_t log_ea, std::uint32_t order_slot) {
    Asm a(nextCode(fx));
    movEaxEspDisp8(a, 8);  // pRN
    movMemEax(a, log_ea + order_slot);
    a.incDword(log_ea + kLogCalled);
    a.movEaxImm(0);
    a.retImm16(16);
    return place(fx, a);
}

// handler C：记 pExcRec->ExceptionFlags（展开相位判据）+ 计数，返 0。
std::uint32_t emitUnwindHandler(Fixture& fx, std::uint32_t log_ea, std::uint32_t counter_slot) {
    Asm a(nextCode(fx));
    movEaxEspDisp8(a, 4);  // pExcRec
    movEcxEaxDisp(a, 0x04);
    movMemEcx(a, log_ea + kLogFlags);
    a.incDword(log_ea + counter_slot);
    a.movEaxImm(0);
    a.retImm16(16);
    return place(fx, a);
}

std::uint32_t rd(const Fixture& fx, std::uint32_t ea) { return fx.m.mem().rd32(ea); }

// =====================================================================================
// A：单 record + guest handler
// =====================================================================================
void caseA() {
    lift_test::g_current_case = "seh/A-single-record";
    Fixture fx;
    LIFT_CHECK(fx.ok);
    if (!fx.ok) return;
    lift::registerBootstrapShims(fx.m.shims());
    const std::uint32_t stub = fx.use("kernel32", "RaiseException");
    LIFT_CHECK(stub != 0);
    if (stub == 0) return;

    const std::uint32_t log_ea = fx.zeroData(kLogBytes);
    const std::uint32_t marker_ea = fx.u32data(0);
    const std::uint32_t after_ea = fx.u32data(0);
    const std::uint32_t info[3] = {kInfo0, kObjectPtr, kThrowInfo};
    const std::uint32_t info_ea = fx.data(info, sizeof(info));
    const std::uint32_t stack_top = fx.m.layout().stack_top;
    // ★ 新栈必须避开两块：(1) driver 自己的参数区（从 stack_top-4 起压 4 个实参：-8..-0x14）；
    //   (2) **宿主铺的 SEH 结构区**（`seh.cpp` 把它放在 entry_esp 之下 0x36C 内 ⇒ 覆盖
    //   stack_top-0x18-0x36C .. stack_top-0x18）。所以取 -0x800（本用例踩过两次）。
    const std::uint32_t new_esp = stack_top - 0x800u;
    fx.m.mem().wr32(new_esp, kSentinel);  // catch 体 `ret` 落到哨兵 ⇒ run 正常收尾

    // catch 体：只做"能被断言"的一件事（真机的 0x41389A 还要复位 translator / 恢复 fs:[0]）
    Asm c(nextCode(fx));
    c.movMemImm(marker_ea, kCatchMarker);
    c.ret();
    const std::uint32_t catch_ea = place(fx, c);

    const std::uint32_t handler_ea = emitHandlerA(fx, log_ea, catch_ea, new_esp);
    const std::uint32_t rec[3] = {lift::kSehChainEnd, handler_ea, 0u};
    const std::uint32_t rec_ea = fx.data(rec, sizeof(rec));
    if (!initTeb(fx, rec_ea)) return;

    std::uint32_t ret_ea = 0;
    const std::uint32_t driver_ea = emitRaiseDriver(fx, stub, info_ea, after_ea, &ret_ea);
    fx.rearmStack();  // esp = stack_top-4，[esp] = 哨兵
    const lift::RunResult r = fx.runCode(driver_ea);

    LIFT_CHECK(r.reason == lift::StopReason::UntilEa);      // A8：同帧 catch 不需要 RtlUnwind
    LIFT_CHECK_EQ_I64(rd(fx, log_ea + kLogCalled), 1);       // A1
    LIFT_CHECK_EQ_U32(rd(fx, log_ea + kLogRN), rec_ea);      // A2
    LIFT_CHECK(rd(fx, log_ea + kLogExcRec) != 0);            // pExcRec = 宿主铺在 guest 栈上的 record
    LIFT_CHECK(rd(fx, log_ea + kLogDC) != 0);
    LIFT_CHECK_EQ_U32(rd(fx, log_ea + kLogCode), kThrowCode);   // A3：逐位透传
    LIFT_CHECK_EQ_U32(rd(fx, log_ea + kLogFlags), kThrowFlags);
    LIFT_CHECK_EQ_U32(rd(fx, log_ea + kLogNargs), 3);
    LIFT_CHECK_EQ_U32(rd(fx, log_ea + kLogInfo0), kInfo0);      // ⛔ 不许合成 0x1994000
    LIFT_CHECK_EQ_U32(rd(fx, log_ea + kLogAddr), ret_ea);        // A4
    LIFT_CHECK_EQ_U32(rd(fx, log_ea + kLogCtxEip), ret_ea);      // A5：初始 CONTEXT = 抛出瞬间
    LIFT_CHECK_EQ_U32(rd(fx, log_ea + kLogCtxEsp), stack_top - 24u);
    LIFT_CHECK_EQ_U32(rd(fx, log_ea + kLogFs0), rec_ea);         // A7：catch 之前链头未变
    LIFT_CHECK_EQ_U32(rd(fx, marker_ea), kCatchMarker);          // A6：真的跳到 catch
    LIFT_CHECK_EQ_U32(rd(fx, after_ea), 0);                      // 没有从 RaiseException 正常返回
    LIFT_CHECK_EQ_U32(rd(fx, fx.m.cpu().fs_base), rec_ea);       // 收尾时链头仍是 record
    LIFT_CHECK_EQ_I64(fx.m.seh().raiseCount(), 1);
    LIFT_CHECK_EQ_I64(fx.m.seh().unwindCount(), 0);              // A8：没调过 RtlUnwind
    LIFT_CHECK(!fx.m.seh().busy());
}

// =====================================================================================
// B：两层 record 都返 0 ⇒ 必须**非 ok** 停机（"不许变静默 no-op"的机器判据）
// =====================================================================================
void caseB() {
    lift_test::g_current_case = "seh/B-unhandled-is-loud";
    Fixture fx;
    LIFT_CHECK(fx.ok);
    if (!fx.ok) return;
    lift::registerBootstrapShims(fx.m.shims());
    const std::uint32_t stub = fx.use("kernel32", "RaiseException");
    LIFT_CHECK(stub != 0);
    if (stub == 0) return;

    const std::uint32_t log_ea = fx.zeroData(kLogBytes);
    const std::uint32_t after_ea = fx.u32data(0);
    const std::uint32_t info[3] = {kInfo0, kObjectPtr, kThrowInfo};
    const std::uint32_t info_ea = fx.data(info, sizeof(info));

    const std::uint32_t h_inner = emitHandlerB(fx, log_ea, kLogOrder0);
    const std::uint32_t h_outer = emitHandlerB(fx, log_ea, kLogOrder1);
    const std::uint32_t rec_outer = fx.zeroData(12);
    const std::uint32_t rec_inner = fx.zeroData(12);
    fx.m.mem().wr32(rec_outer + 0u, lift::kSehChainEnd);
    fx.m.mem().wr32(rec_outer + 4u, h_outer);
    fx.m.mem().wr32(rec_inner + 0u, rec_outer);
    fx.m.mem().wr32(rec_inner + 4u, h_inner);
    if (!initTeb(fx, rec_inner)) return;

    std::uint32_t ret_ea = 0;
    const std::uint32_t driver_ea = emitRaiseDriver(fx, stub, info_ea, after_ea, &ret_ea);
    fx.rearmStack();
    const lift::RunResult r = fx.runCode(driver_ea);

    // ★ 核心判据：走到底 ⇒ **非 ok**（⛔ 不许静默 no-op、不许 exit 0、不许被当成"跑完了"）
    LIFT_CHECK(!r.ok());
    LIFT_CHECK(r.reason == lift::StopReason::UnhandledException);
    LIFT_CHECK(!r.detail.empty());
    LIFT_CHECK(std::strstr(r.detail.c_str(), "SEH") != nullptr || std::strstr(r.detail.c_str(), "链") != nullptr);
    // 链序：内层先、外层后；pRN 分别是两条 record
    LIFT_CHECK_EQ_I64(rd(fx, log_ea + kLogCalled), 2);
    LIFT_CHECK_EQ_U32(rd(fx, log_ea + kLogOrder0), rec_inner);
    LIFT_CHECK_EQ_U32(rd(fx, log_ea + kLogOrder1), rec_outer);
    LIFT_CHECK_EQ_U32(rd(fx, after_ea), 0);  // 没有从 RaiseException 正常返回
    LIFT_CHECK_EQ_I64(fx.m.seh().unhandledCount(), 1);
    LIFT_CHECK(!fx.m.seh().busy());
}

// =====================================================================================
// C：RtlUnwind（flags | 端点 | 两种落点）
// =====================================================================================
void caseC() {
    lift_test::g_current_case = "seh/C-rtlunwind";
    Fixture fx;
    LIFT_CHECK(fx.ok);
    if (!fx.ok) return;
    lift::registerBootstrapShims(fx.m.shims());
    const std::uint32_t stub = fx.use("kernel32", "RtlUnwind");
    LIFT_CHECK(stub != 0);
    if (stub == 0) return;

    const std::uint32_t log_ea = fx.zeroData(kLogBytes);
    const std::uint32_t after_ea = fx.u32data(0);
    const std::uint32_t esp_before_ea = fx.u32data(0);
    const std::uint32_t esp_after_ea = fx.u32data(0);
    const std::uint32_t esp_at_catch_ea = fx.u32data(0);
    const std::uint32_t marker_ea = fx.u32data(0);
    const std::uint32_t stack_top = fx.m.layout().stack_top;

    const std::uint32_t h_inner = emitUnwindHandler(fx, log_ea, kLogCalled);
    const std::uint32_t h_outer = emitUnwindHandler(fx, log_ea, kLogOuter);
    const std::uint32_t rec_outer = fx.zeroData(12);
    const std::uint32_t rec_inner = fx.zeroData(12);
    fx.m.mem().wr32(rec_outer + 0u, lift::kSehChainEnd);
    fx.m.mem().wr32(rec_outer + 4u, h_outer);
    fx.m.mem().wr32(rec_inner + 0u, rec_outer);
    fx.m.mem().wr32(rec_inner + 4u, h_inner);

    // TargetIp 落点（catch 体）：记 esp + 打标记
    Asm cc(nextCode(fx));
    movMemEsp(cc, esp_at_catch_ea);
    cc.movMemImm(marker_ea, kCatchMarker);
    cc.ret();
    const std::uint32_t catch_ea = place(fx, cc);
    // TargetIp 落点用的 esp = "形如被 `ret 16` 返回"的栈顶（= 4 个 push 之前的 esp，也就是
    // `rearmStack()` 写的哨兵那一格）⇒ 不需要另铺哨兵。

    // ---- C.1 TargetIp = 0（正常返回）：`fs:[0] == TargetFrame` + esp 口径 ----
    {
        Asm a(nextCode(fx));
        movMemEsp(a, esp_before_ea);
        a.pushImm(0);          // ReturnValue
        a.pushImm(0);          // ExceptionRecord = NULL（`__global_unwind2` 就是这么传的）
        a.pushImm(0);          // TargetIp = 0 ⇒ 正常返回
        a.pushImm(rec_outer);  // TargetFrame（不含端点：内层 handler 会被调，外层不会）
        a.callRel(stub);
        movMemEsp(a, esp_after_ea);
        a.incDword(log_ea + kLogRetMarker);
        a.ret();
        const std::uint32_t entry = place(fx, a);
        if (!initTeb(fx, rec_inner)) return;
        fx.rearmStack();
        const lift::RunResult r = fx.runCode(entry);
        LIFT_CHECK(r.reason == lift::StopReason::UntilEa);
        LIFT_CHECK_EQ_I64(rd(fx, log_ea + kLogCalled), 1);      // 内层被展开
        LIFT_CHECK_EQ_I64(rd(fx, log_ea + kLogOuter), 0);       // 端点不含
        LIFT_CHECK((rd(fx, log_ea + kLogFlags) & 0x2u) != 0);   // EXCEPTION_UNWINDING
        LIFT_CHECK_EQ_U32(rd(fx, fx.m.cpu().fs_base), rec_outer);  // 链头落到 TargetFrame
        LIFT_CHECK_EQ_U32(rd(fx, log_ea + kLogRetMarker), 1);   // 正常返回（TargetIp=0）
        // "到 TargetIp 时 esp == call 前 + 0x10"：4 个 push 前 esp = E，call 前 = E-0x10，
        // 返回后必须回到 E（形同被 `ret 16` 正常返回）—— 三个调用点都是这个式子。
        LIFT_CHECK(rd(fx, esp_after_ea) == rd(fx, esp_before_ea));
        LIFT_CHECK(rd(fx, esp_before_ea) == stack_top - 4u);
        LIFT_CHECK(!fx.m.seh().busy());
    }

    // ---- C.2 TargetIp != 0：不返回，停在目标；同一 esp 口径 ----
    {
        fx.m.mem().wr32(log_ea + kLogCalled, 0);
        fx.m.mem().wr32(log_ea + kLogOuter, 0);
        fx.m.mem().wr32(after_ea, 0);
        fx.m.mem().wr32(marker_ea, 0);
        Asm a(nextCode(fx));
        movMemEsp(a, esp_before_ea);
        a.pushImm(0);
        a.pushImm(0);
        a.pushImm(catch_ea);   // TargetIp
        a.pushImm(rec_outer);
        a.callRel(stub);
        a.movMemImm(after_ea, kBadReturn);  // TargetIp 路径**不返回** ⇒ 这里不许被执行
        a.ret();
        const std::uint32_t entry = place(fx, a);
        if (!initTeb(fx, rec_inner)) return;
        fx.rearmStack();
        const lift::RunResult r = fx.runCode(entry);
        LIFT_CHECK(r.reason == lift::StopReason::UntilEa);
        LIFT_CHECK_EQ_I64(rd(fx, log_ea + kLogCalled), 1);
        LIFT_CHECK_EQ_U32(rd(fx, after_ea), 0);                 // 没从 call 之后继续跑
        LIFT_CHECK_EQ_U32(rd(fx, marker_ea), kCatchMarker);      // 真的落在 TargetIp
        LIFT_CHECK(rd(fx, esp_at_catch_ea) == rd(fx, esp_before_ea));  // 同一 esp 口径
        LIFT_CHECK_EQ_U32(rd(fx, fx.m.cpu().fs_base), rec_outer);
        LIFT_CHECK(!fx.m.seh().busy());                          // 状态机已清空
    }
}

// =====================================================================================
// D：影子栈同步（展开 = 不经 ret 的跳转 ⇒ 按落地 esp 丢"被跳过的帧"）
// =====================================================================================
void caseD() {
    lift_test::g_current_case = "seh/D-shadow-stack-drop";
    lift::WindowProfiler p;
    p.setArmed(true);
    p.begin("seh-shadow", 0, 0, 0);
    // 调用链：0x100（esp=0x7000）→ 0x200（0x6FF0）→ 0x300（0x6FE0）；栈向下长 ⇒ 越内层 esp 越小
    p.pushCall(0x100u, 0x1010u, 0x7000u);
    p.pushCall(0x200u, 0x2010u, 0x6FF0u);
    p.pushCall(0x300u, 0x3010u, 0x6FE0u);
    LIFT_CHECK_EQ_I64(p.depth(), 3);
    // 落到目标帧 0x200（esp=0x6FF0）⇒ 只丢更内层的 0x300
    LIFT_CHECK_EQ_I64(p.dropFramesBelowEsp(0x6FF0u), 1);
    LIFT_CHECK_EQ_I64(p.depth(), 2);
    // 已经是目标帧自己的 esp ⇒ 一帧都不丢（⛔ 不许把目标帧也丢掉）
    LIFT_CHECK_EQ_I64(p.dropFramesBelowEsp(0x6FE0u), 0);
    LIFT_CHECK_EQ_I64(p.depth(), 2);
    // 落到更外层（esp=0x8000）⇒ 剩下两帧都被跳过
    LIFT_CHECK_EQ_I64(p.dropFramesBelowEsp(0x8000u), 2);
    LIFT_CHECK_EQ_I64(p.depth(), 0);
    LIFT_CHECK_EQ_I64(p.dropFramesBelowEsp(0x9000u), 0);  // 空栈不是错误
    p.end(0, 0, 0);
}

}  // namespace

int main() {
    caseA();
    caseB();
    caseC();
    caseD();
    return lift_test::finish("seh");
}
