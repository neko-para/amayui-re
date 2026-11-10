// emulator-lift · guest 侧 SEH / C++ EH 的**两个 OS 原语**（`RaiseException` / `RtlUnwind`）
//
// ★ 为什么是"两个原语"而不是"一个 C++ EH 展开器"（取证见需求单
//   `REQ-01M708DF24KY0QZNEHY66S0HYW` 的正文 + `.tmp/reveq/<该单>/design.md` +
//   `.tmp/reveq/01M713MSM6WKDDTMHCXR6FJX5Q/report.md`）：
//   AGE.EXE **静态链接** CRT：`_CxxThrowException`(0x4E8A6E) / `___CxxFrameHandler3`(0x4E7094) /
//   `FindHandler` / `CallCatchBlock`(0x4EFFA7) / `_UnwindNestedFrames`(0x4E7040) /
//   `__local_unwind2`(0x506935) 全在镜像里。guest 自己会：
//     ① 从 `.data` 0x52FDE8 的模板抄一份 32 字节 record，并填 `Info = {0x19930520, pObject, pThrowInfo}`；
//     ② `call dword ptr [0x51d1dc]`（0x4E8AB0）把 **code/flags/nargs/args** 原样交给 OS；
//     ③ 在 handler 里按 `_s_FuncInfo` 找 catch、拷 catch 对象，最后 `_JumpToContinuation`(0x4E7007)：
//        `ebp = pRN+0xC` / `esp = [pRN-4]` / `fs:[0] = [fs:[0]]` / `jmp continuation`（**不返回**）。
//   ⇒ 宿主**不解析** FuncInfo / RTTI / TryBlockMap / HandlerType（那些全在 guest），只做 OS 的两件事：
//     - `RaiseException`：在 guest 栈上铺 `EXCEPTION_RECORD`(0x50) / `CONTEXT`(x86, 0x2CC) /
//       `DISPATCHER_CONTEXT`(0x40, 全 0)，沿 `fs:[0]` 链逐帧调 `handler(pExcRec, pRN, pCtx, pDC)`，
//       读 EAX（disposition）：0 = 继续找、1 = 装载 CONTEXT 继续执行、2 = 嵌套（只记一行）。
//     - `RtlUnwind`：`ExceptionFlags |= 2`（EXCEPTION_UNWINDING）、逐帧调 handler、
//       收尾把 `fs:[0]` 落到 `TargetFrame`；`TargetIp != 0` ⇒ **不返回**（跳 TargetIp）。
//
// ★ 四条硬口径（每条都有取证，⛔ 不要凭 x86 常识改）：
//   1. **参数原样透传**：guest 的 `CallCatchBlock` 自己按 `EXCEPTION_RECORD` 的
//      `code@+0x00` / `NumberParameters@+0x10` / `ExceptionInformation[0]@+0x14` 校验
//      （0x4F00F8 / 0x4F0100 / 0x4F0109：`Info[0] ∈ {0x19930520,21,22}`）⇒ 宿主**不许**合成 0x1994000。
//   2. **不在 shim 里跑 guest**：shim 只搬参数 + 把控制交给状态机（`kSehDispatchEa`），
//      handler 帧在**块边界**铺（同 `Machine::fireDueTimer` 的形状；见 machine.cpp 的 `pumpSeh`）。
//   3. **`RtlUnwind` 的 ESP 口径**：全 `.text` 只有 3 个调用点（0x4E7066 / 0x506071 / 0x5068E3），
//      三处都要求"到 TargetIp 时 esp == `call` 之前 + 0x10"（= 形同被 `ret 16` 正常返回）。
//      ⇒ 不猜"目标帧的 ESP"，也不改 EBP/其它寄存器（`_UnwindNestedFrames` 的续点 0x4E706B
//      立刻读自己 `[ebp-0xc]`，ebx/esi/edi 必须还是它自己的）。
//   4. **`TargetFrame` 的值语义**：不含端点（handler 只为"比它更内层"的 record 调）+
//      收尾 `fs:[0] = TargetFrame`（`__global_unwind2` 自己**不**恢复 `fs:[0]`，那是 RtlUnwind 的责任；
//      `_UnwindNestedFrames` 的续点会自己再链一次）。端点判据在夹具 C 里钉住。
//
// ★ 派发期间的两条**宿主侧**不变量（见 machine.cpp 的接线）：
//   · 影子栈（`WindowProfiler`）：展开是"不经 `ret` 的跳转" ⇒ 落地时按"新 esp"丢帧
//     （`dropFramesBelowEsp`）；`RaiseException` 若被 handler 弃帧（`_JumpToContinuation`）
//     也在落地时丢帧 —— ⛔ 不许让 profiler 静默偏大。
//   · 跨 `GuestCallStack` 帧（WndProc / 音频 fill / 定时器回调）⇒ **响亮失败**（Machine 侧判）。
//
// ⛔ 明确不做（本单的边界）：`_global_unwind2` / `_local_unwind2`（它们是 guest 代码）·
//   RTTI / catch 对象拷贝 / `__finally` · VEH / 未处理异常过滤器 / `RtlUnwindEx` / `NtContinue` ·
//   x64 `.pdata` 表驱动展开 · 把宿主 fault 变成 guest SEH。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lift {

struct X86;
class GuestMemory;

// shim → 状态机的一次交接点（dispatcher 在块边界认出它；⛔ 不是 guest 代码页）。
// 与 `kTimerCallbackReturnEa`(0x7FFEF004) / `GuestCallStack::kContinuationEa`(0x7FFEF008) /
// `kernel32_string` 的 0x7FFEF000 / 音频的 0x7FFEF00C 同一组（这组的下两个空位）。
constexpr std::uint32_t kSehDispatchEa = 0x7FFEF010u;
// 合成 handler 帧的返回地址：handler `ret N` 落到这里，dispatcher 在块边界认出它并读 EAX。
constexpr std::uint32_t kSehHandlerReturnEa = 0x7FFEF014u;
// `fs:[0]` 链末端（`image.cpp` 建的 TEB 初值）。走到它就说明"没人接这个异常"。
constexpr std::uint32_t kSehChainEnd = 0xFFFFFFFFu;

// 一次块边界动作的结果（Machine::pumpSeh 据此更新 host 状态 / 停因）。
struct SehStep {
    enum class Kind : std::uint8_t {
        None = 0,      // 没有待办 / handler 正在跑（调用方照常执行）
        CallHandler,   // 已铺好一个 handler 帧（cpu.eip/esp 已写）⇒ 调用方 `continue`
        Transfer,      // 控制已转移到 (eip, esp)（CONTEXT 装载 / RtlUnwind 落点 / handler 弃帧）
        Stop,          // 响亮停：`why` 是 detail
    };
    Kind kind = Kind::None;
    // CallHandler：这一次合成调用的现场（影子栈压帧用；`ret N` 由 guest 自己弹）
    std::uint32_t handler_ea = 0;
    std::uint32_t frame_esp = 0;
    // Transfer：落地后的 (eip, esp)。★ 影子栈同步的**唯一**判据 = esp
    //   （"进入时 esp 落在这个值之下"的帧就是被跳过的帧）。
    std::uint32_t eip = 0;
    std::uint32_t esp = 0;
};

// 派发状态机。一台 `Machine` 一份；**跨块迭代存活**（状态里只有 guest EA 与整数，⛔ 不持宿主指针）。
class Seh {
public:
    // ---- shim 侧（只搬参数；⛔ 不执行 guest）----
    // `entry_esp` = 进入 shim 时的 esp（指向返回地址）；`ret_ea` = 调用点下一条指令。
    // 返回 false ⇒ 已把原因写进 pending stop（调用方仍照常把控制交给 `kSehDispatchEa`，
    // 由 `step()` 报 `Stop`）—— 这是"铺不下结构 / fs_base 没装配"这类**响亮**失败的唯一出口。
    bool beginRaise(X86& cpu, GuestMemory& mem, std::uint32_t code, std::uint32_t flags,
                    std::uint32_t nargs, std::uint32_t args_ea, std::uint32_t entry_esp,
                    std::uint32_t ret_ea);
    bool beginUnwind(X86& cpu, GuestMemory& mem, std::uint32_t target_frame, std::uint32_t target_ip,
                     std::uint32_t exc_rec_ea, std::uint32_t return_value, std::uint32_t entry_esp,
                     std::uint32_t ret_ea);

    // ---- 块边界（Machine::pumpSeh 调）----
    SehStep step(X86& cpu, GuestMemory& mem, std::string* why);

    // 有未完成的派发（含"shim 已交接、等块边界处理"）。
    bool busy() const { return !levels_.empty() || !pending_stop_.empty(); }
    // 正在等一个合成 handler 返回（派发期间不许插输入 tick / 定时器回调 / presenter pump）。
    bool awaitingHandler() const { return !levels_.empty() && levels_.back().awaiting; }
    bool dispatchPending() const { return pending_dispatch_; }
    void setDispatchPending(bool on) { pending_dispatch_ = on; }

    // ---- 每轮 `run()` 开头清一次（状态机跨块存活，⛔ 不许跨 run）----
    void reset();

    // ---- 诊断计数（守卫 / 摘要用；⛔ 不参与判定）----
    std::uint64_t raiseCount() const { return raises_; }
    std::uint64_t unwindCount() const { return unwinds_; }
    std::uint64_t handlerCalls() const { return handler_calls_; }
    std::uint64_t abandonedFrames() const { return abandoned_; }
    std::uint64_t unhandledCount() const { return unhandled_; }

private:
    struct Level {
        enum class Kind : std::uint8_t { Raise = 0, Unwind = 1 } kind = Kind::Raise;
        // 结构区（全部落在 guest 栈：`[rec_ea, +0x50)`、`[ctx_ea, +0x2CC)`、`[dc_ea, +0x40)`）。
        // ★ 宿主各留一份**深拷贝**：展开期间 guest 的 push 可能踩到这些槽（真 Windows 也这样），
        //   每次调 handler 前从宿主副本重铺、返回后读回。
        std::uint32_t rec_ea = 0;
        std::uint32_t ctx_ea = 0;
        std::uint32_t dc_ea = 0;
        std::uint32_t frame_base = 0;   // 结构区底（对齐后）；合成帧从它往下铺
        std::uint32_t rec[0x50 / 4] = {};
        std::uint32_t ctx[0x2CC / 4] = {};
        // record 是**我们自己造的**（Raise / RtlUnwind 传 NULL 时的本地 record）⇒ 每次调 handler 前重铺；
        // 借来的（`RtlUnwind` 的 arg3 = 外层 record）⇒ ⛔ 不回写，只读 flags。
        bool owns_record = true;
        // 游标
        std::uint32_t pRN = 0;            // Raise: 当前要调的 record；Unwind: 正在处理 / 下一个的 record
        std::uint32_t target_frame = 0;   // Unwind：不含端点
        std::uint32_t target_ip = 0;      // Unwind：0 ⇒ 正常返回（eip = ret_ea）
        std::uint32_t return_value = 0;   // Unwind：TargetIp != 0 时写进 EAX
        std::uint32_t ret_ea = 0;         // shim 的返回地址（TargetIp == 0 时回去这里）
        std::uint32_t post_return_esp = 0;  // "形如 stdcall 返回"的 esp（entry + 4 + argc*4）
        // 一次合成 handler 调用
        bool awaiting = false;
        std::uint32_t handler_ea = 0;
        std::uint32_t handler_frame_esp = 0;
    };

    // 结构区：在 `entry_esp` 之下留出 0x50+0x2CC+0x40（+16 余量、16 字节对齐）。
    bool placeStructures(GuestMemory& mem, std::uint32_t entry_esp, Level& L);
    // 宿主副本 → guest（每次调 handler 前）/ guest → 宿主副本（handler 返回后）。
    void emitStructures(GuestMemory& mem, const Level& L) const;
    void absorbStructures(GuestMemory& mem, Level& L) const;
    void snapshotContext(const X86& cpu, Level& L, std::uint32_t eip0, std::uint32_t esp0) const;
    // 整份装载 CONTEXT（8 GPR + EFlags + Eip/Esp + 段选择子）。
    void loadContext(X86& cpu, const Level& L) const;

    // handler 弃帧（guest 自己转移，例如 `_JumpToContinuation`）：丢掉"帧已被弃"的层。
    void pruneAbandoned(const X86& cpu);
    // 一次 handler 调用的铺帧（成功 ⇒ out.kind = CallHandler）。
    void dispatchHandler(X86& cpu, GuestMemory& mem, Level& L, SehStep& out);
    // RtlUnwind 收口：`fs:[0] = TargetFrame`，然后"返回"或"跳 TargetIp"。
    SehStep finishUnwind(X86& cpu, GuestMemory& mem);
    SehStep stop(SehStep out, std::string* why, const std::string& detail);

    std::vector<Level> levels_;
    std::string pending_stop_;   // 非空 ⇒ step() 第一件事就是报 Stop
    bool pending_dispatch_ = false;  // shim 已交出控制（eip = kSehDispatchEa），还没被 step() 认领
    std::uint64_t raises_ = 0;
    std::uint64_t unwinds_ = 0;
    std::uint64_t handler_calls_ = 0;
    std::uint64_t abandoned_ = 0;
    std::uint64_t unhandled_ = 0;
};

}  // namespace lift
