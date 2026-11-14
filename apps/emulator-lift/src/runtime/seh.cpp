// emulator-lift · SEH / C++ EH 派发状态机（`RaiseException` / `RtlUnwind` 的宿主侧）
//
// ★ 读这个文件前先读 `seh.h` 的头注（为什么只有两个原语、四条硬口径、明确不做什么）。
// ★ 本文件里**没有** C++ 语义：FuncInfo / TryBlockMap / RTTI / catch 对象拷贝全在 guest。
//   宿主只做 OS 的事：铺结构（EXCEPTION_RECORD / CONTEXT / DISPATCHER_CONTEXT）→ 沿 `fs:[0]`
//   逐帧调 handler(pExcRec, pRN, pCtx, pDC) → 按 EAX（disposition）决定"继续找 / 装载 CONTEXT / 停"。
#include "runtime/seh.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

#include "runtime/memory.h"
#include "runtime/x86.h"

namespace lift {
namespace {

// ---- EXCEPTION_RECORD（Win32 x86，0x50 字节）----
constexpr std::uint32_t kRecBytes = 0x50;
constexpr std::uint32_t kRecCode = 0x00;
constexpr std::uint32_t kRecFlags = 0x04;
constexpr std::uint32_t kRecExceptionRecord = 0x08;
constexpr std::uint32_t kRecExceptionAddress = 0x0C;
constexpr std::uint32_t kRecNumberParameters = 0x10;
constexpr std::uint32_t kRecInformation = 0x14;  // ExceptionInformation[0..14]
constexpr std::uint32_t kRecMaxParameters = 15;

// ---- CONTEXT（Win32 x86，0x2CC 字节；偏移取自 winnt.h 的 i386 布局）----
constexpr std::uint32_t kCtxBytes = 0x2CC;
constexpr std::uint32_t kCtxContextFlags = 0x00;
constexpr std::uint32_t kCtxSegGs = 0x8C;
constexpr std::uint32_t kCtxSegFs = 0x90;
constexpr std::uint32_t kCtxSegEs = 0x94;
constexpr std::uint32_t kCtxSegDs = 0x98;
constexpr std::uint32_t kCtxEdi = 0x9C;
constexpr std::uint32_t kCtxEsi = 0xA0;
constexpr std::uint32_t kCtxEbx = 0xA4;
constexpr std::uint32_t kCtxEdx = 0xA8;
constexpr std::uint32_t kCtxEcx = 0xAC;
constexpr std::uint32_t kCtxEax = 0xB0;
constexpr std::uint32_t kCtxEbp = 0xB4;
constexpr std::uint32_t kCtxEip = 0xB8;
constexpr std::uint32_t kCtxSegCs = 0xBC;
constexpr std::uint32_t kCtxEFlags = 0xC0;
constexpr std::uint32_t kCtxEsp = 0xC4;
constexpr std::uint32_t kCtxSegSs = 0xC8;
// CONTEXT_i386(0x00010000) | CONTEXT_CONTROL(1) | CONTEXT_INTEGER(2) | CONTEXT_SEGMENTS(4)
constexpr std::uint32_t kContextFull = 0x00010007u;

// ---- DISPATCHER_CONTEXT（x86 口径；本单只给一块**全 0**，见 seh.h 与需求单的裁决）----
constexpr std::uint32_t kDcBytes = 0x40;

// disposition（`EXCEPTION_DISPOSITION`）
constexpr std::uint32_t kExceptionContinueSearch = 0;     // 这个 handler 不接
constexpr std::uint32_t kExceptionContinueExecution = 1;  // 接了：装载 CONTEXT 继续执行
constexpr std::uint32_t kExceptionNestedException = 2;    // handler 里又抛了（本单只记一行）

constexpr std::uint32_t kExceptionUnwinding = 0x2u;  // EXCEPTION_UNWINDING

// ---- 「非连续异常不许继续执行」-----------------------------------------------
// `winnt.h`：`EXCEPTION_NONCONTINUABLE = 0x01`；`EXCEPTION_NONCONTINUABLE_EXCEPTION = 0xC0000025`。
//
// ★ 文档口径（⛔ 不要按"handler 改了 CONTEXT 就可以继续"的直觉改；实测见下）：
//   · `RaiseException` 的 `dwExceptionFlags`：`EXCEPTION_NONCONTINUABLE` = 非连续异常，
//     "Any attempt to continue execution after a noncontinuable exception causes the
//      EXCEPTION_NONCONTINUABLE_EXCEPTION exception."
//     <https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-raiseexception>
//   · `0xC0000025` = `STATUS_NONCONTINUABLE_EXCEPTION`，默认文本
//     "{EXCEPTION} Cannot Continue Windows cannot continue from this exception."
//     <https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-erref/596a1078-e883-4972-9bbc-49e60bebca55>
//
// ★ 判据是**旗标**、不是"CONTEXT 有没有被改"（实测 2026-11-14，本机 x86-32 原生探针：
//   `__try/__except` 的 filter 返回 `EXCEPTION_CONTINUE_EXECUTION`，即 handler 侧
//   `ExceptionContinueExecution`）：
//     flags=0x1 + CONTEXT 未改 ⇒ 第二次进 filter：`code=0xC0000025 flags=0x1 nargs=0`（改抛）；
//     flags=0x1 + filter **改了** CONTEXT（Eip/Esp 指向落点）⇒ **同样**改抛 0xC0000025；
//     flags=0x0 + 改了 CONTEXT ⇒ 落在 filter 给的 Eip（合法：这是 continuable 的文档行为）；
//     flags=0x0 + 未改 CONTEXT ⇒ `RaiseException` 正常返回（合法：continuable 可以"吞"）。
//   ⇒ 所以 MSVC 的 C++ `catch` **不能**靠"返回 1 + 改 CONTEXT"实现（`_CxxThrowException` 抛的
//     全是 `flags=1`）—— 它走 `_JumpToContinuation`（0x4E7007：`ebp=pRN+0xC` / `esp=[pRN-4]` /
//     `jmp continuation`，**不返回**）。本线的"handler 弃帧"路径接的就是它。
//
// ★ 本线的处置：**响亮停**（`SehStep::noncontinuable_violation`），⛔ 不装载 CONTEXT。
//   真机在这一点上会**改抛** 0xC0000025 并把新 record 再走一遍 `fs:[0]` 链；本线不建模那次
//   第二次派发（边界见 `seh.h` 的"明确不做"与 `docs/03-emulator/design-notes.md` 的 L3-seh-01）。
//   停因要**指名道姓**（`0xC0000025` / `EXCEPTION_NONCONTINUABLE_EXCEPTION`），
//   ⛔ 不许退化成"装载 CONTEXT 继续跑"再落进不知哪里的 `int3`（`REQ-01M72GAA7CVKBPP9QRT0PRPBTM`）。
constexpr std::uint32_t kExceptionNonContinuable = 0x1u;
constexpr std::uint32_t kExceptionNonContinuableException = 0xC0000025u;

// 合成 handler 帧：4 个参数 + 返回地址（stdcall；handler 自己 `ret 16`）。
constexpr std::uint32_t kHandlerArgBytes = 4u * 4u;
constexpr std::uint32_t kHandlerFrameBytes = kHandlerArgBytes + 4u;

// 结构区总长 + 对齐余量（全部落在 guest 栈上，⛔ 不进 guest 静态区）。
constexpr std::uint32_t kStructBytes = kRecBytes + kCtxBytes + kDcBytes + 0x10u;

inline std::uint32_t wordAt(const std::uint32_t* w, std::uint32_t off) { return w[off >> 2]; }
inline void setWord(std::uint32_t* w, std::uint32_t off, std::uint32_t v) { w[off >> 2] = v; }

// `LIFT_SEH_TRACE=1` ⇒ 把派发过程打到 stderr（诊断；缺省关 ⇒ 确定性输出不受影响）。
bool sehTraceEnabled() {
    static const bool on = std::getenv("LIFT_SEH_TRACE") != nullptr;
    return on;
}

void sehLog(const char* fmt, ...) {
    if (!sehTraceEnabled()) return;
    std::fprintf(stderr, "[seh] ");
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
}

// 取证用：逐字节打一块 `size` 字节（`LIFT_SEH_TRACE` 关时一行都不打、也不算）。
// ★ 只在 `disposition=1` 的**非连续**分支调用（罕见）：`REQ-01M72GAA7CVKBPP9QRT0PRPBTM`
//   要的"handler 前后 ctx_ea 字节对比"就靠它，省得下次再造一次临时诊断。
void sehDumpBytes(const char* tag, std::uint32_t base_ea, const std::uint32_t* words,
                  std::uint32_t bytes) {
    if (!sehTraceEnabled()) return;
    for (std::uint32_t off = 0; off < bytes; off += 16u) {
        char line[16 * 3 + 1];
        char* p = line;
        const std::uint32_t n = bytes - off < 16u ? bytes - off : 16u;
        for (std::uint32_t i = 0; i < n; ++i) {
            const std::uint32_t byte_off = off + i;  // 字内小端：byte = (word >> 8*(off&3)) & 0xFF
            const std::uint32_t byte = (words[byte_off >> 2] >> (8u * (byte_off & 3u))) & 0xFFu;
            p += std::snprintf(p, 4, "%02X ", byte);
        }
        *p = '\0';
        sehLog("  %s ctx_ea+0x%03X: %s", tag, off, line);
    }
    sehLog("  %s ctx_ea=0x%08X（共 0x%X 字节）", tag, base_ea, bytes);
}

}  // namespace

void Seh::reset() {
    levels_.clear();
    pending_stop_.clear();
    pending_dispatch_ = false;
}

// ---------------------------------------------------------------------------------------
// 结构区：从 `entry_esp` **往下**留一块（真 Windows 也是把 record 建在栈上）。返回 false 时
// `pending_stop_` 已写明原因（调用方照常交给 `kSehDispatchEa`，由 `step()` 响亮停）。
// ---------------------------------------------------------------------------------------
bool Seh::placeStructures(GuestMemory& mem, std::uint32_t entry_esp, Level& L) {
    if (entry_esp <= kStructBytes + 0x1000u) {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "guest 栈不够铺 SEH 结构（esp=0x%08X，需要 0x%X）", entry_esp, kStructBytes);
        pending_stop_ = buf;
        return false;
    }
    const std::uint32_t base = (entry_esp - kStructBytes) & ~0x0Fu;
    if (!mem.committed(base, kRecBytes + kCtxBytes + kDcBytes)) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "SEH 结构区不可写（0x%08X..+0x%X）", base,
                      kRecBytes + kCtxBytes + kDcBytes);
        pending_stop_ = buf;
        return false;
    }
    L.frame_base = base;
    L.rec_ea = base;
    L.ctx_ea = base + kRecBytes;
    L.dc_ea = base + kRecBytes + kCtxBytes;
    return true;
}

void Seh::emitStructures(GuestMemory& mem, const Level& L) const {
    if (L.owns_record) {
        for (std::uint32_t off = 0; off < kRecBytes; off += 4) mem.wr32(L.rec_ea + off, wordAt(L.rec, off));
    }
    for (std::uint32_t off = 0; off < kCtxBytes; off += 4) mem.wr32(L.ctx_ea + off, wordAt(L.ctx, off));
    // DISPATCHER_CONTEXT 全 0（本单不消费它；每次重铺，防展开期的 push 踩到）。
    for (std::uint32_t off = 0; off < kDcBytes; off += 4) mem.wr32(L.dc_ea + off, 0u);
}

void Seh::absorbStructures(GuestMemory& mem, Level& L) const {
    if (L.owns_record) {
        for (std::uint32_t off = 0; off < kRecBytes; off += 4) setWord(L.rec, off, mem.rd32(L.rec_ea + off));
    } else {
        // 借来的 record（`RtlUnwind` 的 arg3）：只读回 flags 供日志，⛔ 不回写（它是 guest 的）。
        setWord(L.rec, kRecFlags, mem.rd32(L.rec_ea + kRecFlags));
    }
    for (std::uint32_t off = 0; off < kCtxBytes; off += 4) setWord(L.ctx, off, mem.rd32(L.ctx_ea + off));
}

// 初始 CONTEXT = **抛出瞬间**的快照（契约，不是 C++ 语义）：
//   Eip = 调用点下一条（`ret_ea`，也是 `EXCEPTION_RECORD.ExceptionAddress`）、Esp = 进入 shim 时的 esp。
// ★ 这条口径由夹具 A5 钉住（⛔ 不许用"派发中途"的 esp）。
void Seh::snapshotContext(const X86& cpu, Level& L, std::uint32_t eip0, std::uint32_t esp0) const {
    setWord(L.ctx, kCtxContextFlags, kContextFull);
    setWord(L.ctx, kCtxEdi, cpu.edi());
    setWord(L.ctx, kCtxEsi, cpu.esi());
    setWord(L.ctx, kCtxEbx, cpu.ebx());
    setWord(L.ctx, kCtxEdx, cpu.edx());
    setWord(L.ctx, kCtxEcx, cpu.ecx());
    setWord(L.ctx, kCtxEax, cpu.eax());
    setWord(L.ctx, kCtxEbp, cpu.ebp());
    setWord(L.ctx, kCtxEip, eip0);
    setWord(L.ctx, kCtxEFlags, cpu.eflags);
    setWord(L.ctx, kCtxEsp, esp0);
    setWord(L.ctx, kCtxSegCs, cpu.seg[1]);
    setWord(L.ctx, kCtxSegSs, cpu.seg[2]);
    setWord(L.ctx, kCtxSegDs, cpu.seg[3]);
    setWord(L.ctx, kCtxSegEs, cpu.seg[0]);
    setWord(L.ctx, kCtxSegFs, cpu.seg[4]);
    setWord(L.ctx, kCtxSegGs, cpu.seg[5]);
}

// 整份装载 CONTEXT（真机 `RtlRestoreContext` 是整份）：8 个 GPR + EFLAGS + Eip/Esp + 段选择子。
// ⛔ 只还 eip/esp 会把 handler 的浮财漏进 catch 体。
void Seh::loadContext(X86& cpu, const Level& L) const {
    cpu.edi() = wordAt(L.ctx, kCtxEdi);
    cpu.esi() = wordAt(L.ctx, kCtxEsi);
    cpu.ebx() = wordAt(L.ctx, kCtxEbx);
    cpu.edx() = wordAt(L.ctx, kCtxEdx);
    cpu.ecx() = wordAt(L.ctx, kCtxEcx);
    cpu.eax() = wordAt(L.ctx, kCtxEax);
    cpu.ebp() = wordAt(L.ctx, kCtxEbp);
    cpu.eip = wordAt(L.ctx, kCtxEip);
    cpu.eflags = wordAt(L.ctx, kCtxEFlags);
    cpu.esp() = wordAt(L.ctx, kCtxEsp);
    cpu.seg[1] = static_cast<std::uint16_t>(wordAt(L.ctx, kCtxSegCs));
    cpu.seg[2] = static_cast<std::uint16_t>(wordAt(L.ctx, kCtxSegSs));
    cpu.seg[3] = static_cast<std::uint16_t>(wordAt(L.ctx, kCtxSegDs));
    cpu.seg[0] = static_cast<std::uint16_t>(wordAt(L.ctx, kCtxSegEs));
    cpu.seg[4] = static_cast<std::uint16_t>(wordAt(L.ctx, kCtxSegFs));
    cpu.seg[5] = static_cast<std::uint16_t>(wordAt(L.ctx, kCtxSegGs));
}

// handler 弃帧 = guest 自己转移（取证：`_JumpToContinuation` 0x4E7007 把
//   `ebp = pRN+0xC` / `esp = [pRN-4]` / `jmp continuation`，⛔ **不经过 ret**）。
// 判据：handler 的帧只可能向下长 ⇒ 块边界上 esp 高于它的进入 esp，说明那帧已经被丢掉。
void Seh::pruneAbandoned(const X86& cpu) {
    while (!levels_.empty() && levels_.back().awaiting &&
           cpu.esp() > levels_.back().handler_frame_esp) {
        sehLog("handler 0x%08X 弃帧（esp=0x%08X > 帧底 0x%08X）⇒ 这一层派发结束",
               levels_.back().handler_ea, cpu.esp(), levels_.back().handler_frame_esp);
        levels_.pop_back();
        ++abandoned_;
    }
}

bool Seh::beginRaise(X86& cpu, GuestMemory& mem, std::uint32_t code, std::uint32_t flags,
                     std::uint32_t nargs, std::uint32_t args_ea, std::uint32_t entry_esp,
                     std::uint32_t ret_ea) {
    pending_dispatch_ = true;
    pruneAbandoned(cpu);
    // ★ 派发深度上限 1（需求单的裁决）：handler 里再抛 = 嵌套异常；本线不做，⛔ 不许静默。
    if (!levels_.empty()) {
        char buf[200];
        std::snprintf(buf, sizeof(buf),
                      "嵌套 RaiseException（已有 %zu 层派发在等 handler 返回）⇒ 本线不支持嵌套异常",
                      levels_.size());
        pending_stop_ = buf;
        return false;
    }
    if (cpu.fs_base == 0) {
        pending_stop_ = "fs_base 未装配（TEB 不在）⇒ 读不到 fs:[0] 的 SEH 链";
        return false;
    }
    Level L;
    L.kind = Level::Kind::Raise;
    L.ret_ea = ret_ea;
    L.post_return_esp = entry_esp + 4u + 4u * 4u;  // RaiseException stdcall 4 参
    if (!placeStructures(mem, entry_esp, L)) return false;
    const std::uint32_t head = mem.rd32(cpu.fs_base);
    if (head == kSehChainEnd || head == 0) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "fs:[0] 已经是链末端（0x%08X）⇒ 这个异常没有 handler", head);
        pending_stop_ = buf;
        return false;
    }
    // ★ 原样透传给 guest（⛔ 不许合成 0x1994000：guest 在 0x4F00F8/0x4F0100/0x4F0109 自己校验）。
    const std::uint32_t n = nargs > kRecMaxParameters ? kRecMaxParameters : nargs;
    if (args_ea != 0 && n != 0 && !mem.committed(args_ea, static_cast<std::size_t>(n) * 4u)) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "lpArguments 不可读（0x%08X，nargs=%u）", args_ea, n);
        pending_stop_ = buf;
        return false;
    }
    setWord(L.rec, kRecCode, code);
    setWord(L.rec, kRecFlags, flags);
    setWord(L.rec, kRecExceptionRecord, 0u);
    setWord(L.rec, kRecExceptionAddress, ret_ea);
    setWord(L.rec, kRecNumberParameters, n);
    for (std::uint32_t i = 0; i < n; ++i) {
        setWord(L.rec, kRecInformation + 4u * i, args_ea != 0 ? mem.rd32(args_ea + 4u * i) : 0u);
    }
    snapshotContext(cpu, L, ret_ea, entry_esp);
    L.pRN = head;
    emitStructures(mem, L);
    sehLog("RaiseException(code=0x%08X flags=0x%X nargs=%u) rec=0x%08X ctx=0x%08X 链头=0x%08X",
           code, flags, n, L.rec_ea, L.ctx_ea, head);
    // ★ 诊断（只在 LIFT_SEH_TRACE）：把链头每一环的**区域归属**打出来。
    //   判据：链头 / handler 落在 Heap / Stack / Teb 上 ⇒ 这条链不是"当前线程的代码栈链"
    //   （真 Windows 的 fs:[0] 是**每线程**的；共用 TEB 会让另一个线程的 record 出现在这里）。
    if (head != kSehChainEnd) {
        std::uint32_t rn = head;
        for (int i = 0; i < 8 && rn != 0 && rn != kSehChainEnd; ++i) {
            if (!mem.committed(rn, 8)) {
                sehLog("  链[%d] record=0x%08X 不可读 ⇒ 停", i, rn);
                break;
            }
            const std::uint32_t handler = mem.rd32(rn + 4u);
            const Region* rr = mem.regionOf(rn);
            const Region* hr = mem.regionOf(handler);
            sehLog("  链[%d] record=0x%08X region=%s handler=0x%08X region=%s", i, rn,
                   rr ? regionKindName(rr->kind) : "(未登记)", handler,
                   hr ? regionKindName(hr->kind) : "(未登记)");
            rn = mem.rd32(rn);
        }
    }
    levels_.push_back(L);
    ++raises_;
    return true;
}

bool Seh::beginUnwind(X86& cpu, GuestMemory& mem, std::uint32_t target_frame, std::uint32_t target_ip,
                      std::uint32_t exc_rec_ea, std::uint32_t return_value, std::uint32_t entry_esp,
                      std::uint32_t ret_ea) {
    pending_dispatch_ = true;
    pruneAbandoned(cpu);
    // RtlUnwind 可以嵌在 Raise 的 handler 里（取证：`CatchIt` → `_UnwindNestedFrames` → 0x4E7066），
    // 也允许嵌在另一次展开里；只挡"深到不像话"的情况（防 host 栈失控）。
    if (levels_.size() >= 16) {
        pending_stop_ = "RtlUnwind 嵌套过深（>=16 层）";
        return false;
    }
    if (cpu.fs_base == 0) {
        pending_stop_ = "fs_base 未装配（TEB 不在）⇒ 读不到 fs:[0] 的 SEH 链";
        return false;
    }
    Level L;
    L.kind = Level::Kind::Unwind;
    L.ret_ea = ret_ea;
    L.post_return_esp = entry_esp + 4u + 4u * 4u;  // RtlUnwind stdcall 4 参
    L.target_frame = target_frame;
    L.target_ip = target_ip;
    L.return_value = return_value;
    if (!placeStructures(mem, entry_esp, L)) return false;
    if (exc_rec_ea != 0) {
        if (!mem.committed(exc_rec_ea, kRecBytes)) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "RtlUnwind 的 ExceptionRecord 不可读（0x%08X）", exc_rec_ea);
            pending_stop_ = buf;
            return false;
        }
        L.rec_ea = exc_rec_ea;
        L.owns_record = false;  // 借来的：⛔ 不回写（它是 guest / 外层派发的）
        setWord(L.rec, kRecFlags, mem.rd32(exc_rec_ea + kRecFlags) | kExceptionUnwinding);
    } else {
        // `__global_unwind2` 就传 NULL（0x5068D9）⇒ 按文档造一条本地 record 再用它。
        setWord(L.rec, kRecCode, 0u);
        setWord(L.rec, kRecFlags, kExceptionUnwinding);
        setWord(L.rec, kRecExceptionRecord, 0u);
        setWord(L.rec, kRecExceptionAddress, ret_ea);
        setWord(L.rec, kRecNumberParameters, 0u);
    }
    // ★ 展开相位：guest 的 handler 靠 `ExceptionFlags & (2|4)` 区分"找 handler"与"展开"
    //   （证据：`__unwind_handler` 0x5068F4 `test dword ptr [ecx+4], 6`）。
    mem.wr32(L.rec_ea + kRecFlags, wordAt(L.rec, kRecFlags));
    for (Level& outer : levels_) {
        if (outer.rec_ea == L.rec_ea) setWord(outer.rec, kRecFlags, wordAt(L.rec, kRecFlags));
    }
    snapshotContext(cpu, L, ret_ea, entry_esp);
    L.pRN = mem.rd32(cpu.fs_base);
    emitStructures(mem, L);
    sehLog("RtlUnwind(targetFrame=0x%08X targetIp=0x%08X rec=0x%08X) 链头=0x%08X",
           target_frame, target_ip, L.rec_ea, L.pRN);
    levels_.push_back(L);
    ++unwinds_;
    return true;
}

SehStep Seh::stop(SehStep out, std::string* why, const std::string& detail) {
    out.kind = SehStep::Kind::Stop;
    if (why) *why = detail;
    sehLog("停：%s", detail.c_str());
    return out;
}

// RtlUnwind 收口：`fs:[0] = TargetFrame`（`__global_unwind2` 自己**不**恢复链头 ⇒ 这是本函数的责任），
// 然后 TargetIp != 0 ⇒ 跳到它（esp 形同被 `ret 16` 正常返回，取证 3/3 个调用点）；否则正常返回。
SehStep Seh::finishUnwind(X86& cpu, GuestMemory& mem) {
    const Level L = levels_.back();
    levels_.pop_back();
    mem.wr32(cpu.fs_base, L.target_frame);
    const std::uint32_t eip = L.target_ip != 0 ? L.target_ip : L.ret_ea;
    cpu.esp() = L.post_return_esp;
    cpu.eip = eip;
    if (L.target_ip != 0) cpu.eax() = L.return_value;  // 文档口径；本镜像三个调用点都传 0
    SehStep out;
    out.kind = SehStep::Kind::Transfer;
    out.eip = eip;
    out.esp = cpu.esp();
    sehLog("RtlUnwind 收口：fs:[0]=0x%08X，落点 eip=0x%08X esp=0x%08X（%s）", L.target_frame, eip,
           cpu.esp(), L.target_ip != 0 ? "TargetIp：不返回" : "TargetIp=0：正常返回");
    return out;
}

// 铺一个合成 handler 帧：handler(pExcRec, pRN, pCtx, pDC)（stdcall），返回地址 = kSehHandlerReturnEa。
void Seh::dispatchHandler(X86& cpu, GuestMemory& mem, Level& L, SehStep& out) {
    // ★ 展开相位：每次调 handler 前都保证 `ExceptionFlags & 2`（guest 的 handler 靠它区分
    //   "第一次找 handler" 与 "展开"；证据：`__unwind_handler` 0x5068F4 `test dword ptr [ecx+4], 6`）。
    //   借来的 record（`RtlUnwind` 的 arg3）也要写回 guest —— 它可能已被别的 handler 改过。
    if (L.kind == Level::Kind::Unwind) {
        setWord(L.rec, kRecFlags, wordAt(L.rec, kRecFlags) | kExceptionUnwinding);
        if (!L.owns_record) mem.wr32(L.rec_ea + kRecFlags, wordAt(L.rec, kRecFlags));
    }
    emitStructures(mem, L);  // 展开期的 push 可能踩到结构区 ⇒ 每次调用前从宿主深拷贝重铺
    // 发出前留一份 CONTEXT：handler 返回后要比"它到底有没有改"（只进日志/停因，⛔ 不参与判定）。
    for (std::uint32_t off = 0; off < kCtxBytes; off += 4) setWord(L.ctx_at_call, off, wordAt(L.ctx, off));
    const std::uint32_t handler = mem.rd32(L.pRN + 4u);
    const std::uint32_t frame = L.frame_base - kHandlerFrameBytes;
    mem.wr32(frame + 0x00, kSehHandlerReturnEa);
    mem.wr32(frame + 0x04, L.rec_ea);
    mem.wr32(frame + 0x08, L.pRN);
    mem.wr32(frame + 0x0C, L.ctx_ea);
    mem.wr32(frame + 0x10, L.dc_ea);
    cpu.esp() = frame;
    cpu.eip = handler;
    L.awaiting = true;
    L.handler_ea = handler;
    L.handler_frame_esp = frame;
    ++handler_calls_;
    out.kind = SehStep::Kind::CallHandler;
    out.handler_ea = handler;
    out.frame_esp = frame;
    sehLog("%s 调 handler 0x%08X（pRN=0x%08X，帧 esp=0x%08X）",
           L.kind == Level::Kind::Raise ? "找 handler" : "展开", handler, L.pRN, frame);
}

SehStep Seh::step(X86& cpu, GuestMemory& mem, std::string* why) {
    SehStep out;
    // 0) shim 侧已经判定"铺不下 / 装配错"⇒ 第一次块边界就响亮停（⛔ 不许静默 no-op）。
    if (!pending_stop_.empty()) {
        const std::string detail = "SEH 派发失败：" + pending_stop_;
        pending_stop_.clear();
        levels_.clear();
        pending_dispatch_ = false;
        ++unhandled_;
        return stop(out, why, detail);
    }
    if (levels_.empty()) {
        pending_dispatch_ = false;
        return out;
    }
    // 一次块边界最多推进这么多步（防 handler 链打转；正常路径 1~2 步）。
    for (int guard = 0; guard < 256; ++guard) {
        Level& L = levels_.back();
        // ---- 1) 等 handler 返回 ----
        if (L.awaiting) {
            if (cpu.eip != kSehHandlerReturnEa) {
                if (cpu.esp() > L.handler_frame_esp) {
                    // handler 弃帧（guest 自己转移；见 `pruneAbandoned` 的取证）。
                    const std::uint32_t eip = cpu.eip;
                    const std::uint32_t esp = cpu.esp();
                    sehLog("handler 0x%08X 弃帧 ⇒ 派发结束，落到 eip=0x%08X esp=0x%08X",
                           L.handler_ea, eip, esp);
                    levels_.clear();
                    ++abandoned_;
                    out.kind = SehStep::Kind::Transfer;
                    out.eip = eip;
                    out.esp = esp;
                    return out;
                }
                return out;  // handler 还在跑
            }
            const std::uint32_t disposition = cpu.eax();
            absorbStructures(mem, L);  // handler 对 record / CONTEXT 的写回读到宿主副本
            L.awaiting = false;
            sehLog("handler 0x%08X 返回 disposition=%u", L.handler_ea, disposition);
            if (L.kind == Level::Kind::Raise) {
                if (disposition == kExceptionContinueExecution) {
                    // ★ 硬口径 5（取证见本文件 `kExceptionNonContinuable` 的注释与 seh.h）：
                    //   `EXCEPTION_NONCONTINUABLE` 上"继续执行"**不成立** —— 真机在这一步**改抛**
                    //   `EXCEPTION_NONCONTINUABLE_EXCEPTION`(0xC0000025)，⛔ 不是装载 CONTEXT。
                    //   判据是**旗标**（CONTEXT 改没改都一样，实测改了也照抛）。
                    //   ⇒ 本线响亮停（独立停因），⛔ 不许静默继续：装载 CONTEXT 会把异常丢在
                    //   raise 的下一条上（`REQ-01M72GAA7CVKBPP9QRT0PRPBTM` 的现场）。
                    const std::uint32_t rec_flags = wordAt(L.rec, kRecFlags);
                    if ((rec_flags & kExceptionNonContinuable) != 0) {
                        std::uint32_t changed = 0;
                        std::uint32_t first_off = 0;
                        std::uint32_t first_old = 0;
                        std::uint32_t first_new = 0;
                        for (std::uint32_t off = 0; off < kCtxBytes; off += 4) {
                            const std::uint32_t now = wordAt(L.ctx, off);
                            if (now == wordAt(L.ctx_at_call, off)) continue;
                            if (changed == 0) {
                                first_off = off;
                                first_old = wordAt(L.ctx_at_call, off);
                                first_new = now;
                            }
                            ++changed;
                        }
                        // 取证（`LIFT_SEH_TRACE=1` 才打）：handler 前后 `ctx_ea` 的逐字节原文。
                        sehDumpBytes("调用前", L.ctx_ea, L.ctx_at_call, kCtxBytes);
                        sehDumpBytes("返回后", L.ctx_ea, L.ctx, kCtxBytes);
                        sehLog("  [pRN+8]=0x%08X（guest 的 try 状态字）；宿主只读 [pRN] / [pRN+4]",
                               mem.rd32(L.pRN + 8u));
                        char buf[1024];
                        if (changed == 0) {
                            std::snprintf(
                                buf, sizeof(buf),
                                "非连续异常被要求继续执行（EXCEPTION_NONCONTINUABLE_EXCEPTION / "
                                "0xC0000025）：record code=0x%08X flags=0x%X 含 EXCEPTION_NONCONTINUABLE，"
                                "而 handler 0x%08X 返回 disposition=1（ExceptionContinueExecution）⇒ "
                                "文档：**任何**继续执行的尝试都会改抛 "
                                "EXCEPTION_NONCONTINUABLE_EXCEPTION(0xC0000025)（⛔ 不是装载 CONTEXT "
                                "继续跑）。★ 现场：CONTEXT **一个字节都没被改**（0x%X 字节逐字比对全等），"
                                "它的 eip=0x%08X 就是 raise 的下一条 ⇒ 照字面装载 = 让异常被静默丢弃。"
                                "本线不建模改抛后的第二次派发 ⇒ 在这里响亮停。",
                                wordAt(L.rec, kRecCode), rec_flags, L.handler_ea, kCtxBytes,
                                wordAt(L.ctx, kCtxEip));
                        } else {
                            std::snprintf(
                                buf, sizeof(buf),
                                "非连续异常被要求继续执行（EXCEPTION_NONCONTINUABLE_EXCEPTION / "
                                "0xC0000025）：record code=0x%08X flags=0x%X 含 EXCEPTION_NONCONTINUABLE，"
                                "而 handler 0x%08X 返回 disposition=1（ExceptionContinueExecution）⇒ "
                                "文档：**任何**继续执行的尝试都会改抛 "
                                "EXCEPTION_NONCONTINUABLE_EXCEPTION(0xC0000025)（⛔ 不是装载 CONTEXT "
                                "继续跑）。★ handler 改了 CONTEXT（%u 字，首处 +0x%X：0x%08X → 0x%08X）"
                                "也**照样**改抛（判据是旗标）。本线不建模改抛后的第二次派发 ⇒ "
                                "在这里响亮停。",
                                wordAt(L.rec, kRecCode), rec_flags, L.handler_ea, changed, first_off,
                                first_old, first_new);
                        }
                        levels_.clear();
                        pending_dispatch_ = false;
                        ++unhandled_;
                        out.noncontinuable_violation = true;
                        return stop(out, why, buf);
                    }
                    loadContext(cpu, L);
                    out.kind = SehStep::Kind::Transfer;
                    out.eip = cpu.eip;
                    out.esp = cpu.esp();
                    levels_.pop_back();
                    sehLog("disposition=1（可继续的异常）⇒ 整份装载 CONTEXT：eip=0x%08X esp=0x%08X",
                           out.eip, out.esp);
                    return out;
                }
                if (disposition == kExceptionNestedException) {
                    sehLog("disposition=2（nested exception）：本单不做语义，按 continue-search 往下走");
                } else if (disposition != kExceptionContinueSearch) {
                    sehLog("未知 disposition=%u ⇒ 按 continue-search 处理", disposition);
                }
                // 推进到链上的下一条 record（**不动 fs:[0]**：正常派发不改链头）。
                L.pRN = mem.rd32(L.pRN);
                if (L.pRN == kSehChainEnd || L.pRN == 0) {
                    char buf[240];
                    std::snprintf(buf, sizeof(buf),
                                  "guest SEH 链走到底（未处理异常）：fs:[0] 链上没有 handler 接受 code=0x%08X"
                                  "（record 0x%08X 的 Next=0x%08X）",
                                  wordAt(L.rec, kRecCode), L.rec_ea, L.pRN);
                    levels_.clear();
                    pending_dispatch_ = false;
                    ++unhandled_;
                    return stop(out, why, buf);
                }
                continue;  // 去派发下一条
            }
            // ---- Unwind：handler 在展开相位返回 ⇒ 处理下一条 ----
            if (disposition != kExceptionContinueSearch) {
                sehLog("展开相位的 handler 0x%08X 返回 disposition=%u（忽略；展开由 TargetFrame 收口）",
                       L.handler_ea, disposition);
            }
            L.pRN = mem.rd32(L.pRN);
            continue;
        }
        // ---- 2) 需要派发 / 收口 ----
        if (L.kind == Level::Kind::Unwind) {
            if (L.pRN == L.target_frame) return finishUnwind(cpu, mem);
            if (L.pRN == kSehChainEnd || L.pRN == 0) {
                char buf[240];
                std::snprintf(buf, sizeof(buf),
                              "RtlUnwind: TargetFrame 0x%08X 不在 fs:[0] 链上（走到 0x%08X 就到底了）",
                              L.target_frame, L.pRN);
                levels_.clear();
                pending_dispatch_ = false;
                ++unhandled_;
                return stop(out, why, buf);
            }
        } else if (L.pRN == kSehChainEnd || L.pRN == 0) {
            char buf[240];
            std::snprintf(buf, sizeof(buf),
                          "guest SEH 链走到底（未处理异常）：链头就是末端 0x%08X（没有 handler 可调）",
                          L.pRN);
            levels_.clear();
            pending_dispatch_ = false;
            ++unhandled_;
            return stop(out, why, buf);
        }
        if (!mem.committed(L.pRN, 12u)) {
            char buf[200];
            std::snprintf(buf, sizeof(buf), "EXCEPTION_REGISTRATION_RECORD 不可读（0x%08X）", L.pRN);
            levels_.clear();
            pending_dispatch_ = false;
            ++unhandled_;
            return stop(out, why, buf);
        }
        if (mem.rd32(L.pRN + 4u) == 0) {
            char buf[200];
            std::snprintf(buf, sizeof(buf), "EXCEPTION_REGISTRATION_RECORD.Handler == 0（record 0x%08X）",
                          L.pRN);
            levels_.clear();
            pending_dispatch_ = false;
            ++unhandled_;
            return stop(out, why, buf);
        }
        dispatchHandler(cpu, mem, L, out);
        return out;
    }
    levels_.clear();
    pending_dispatch_ = false;
    ++unhandled_;
    return stop(out, why, "SEH 派发状态机在一次块边界里转了 256 步（handler 链打转）");
}

}  // namespace lift
