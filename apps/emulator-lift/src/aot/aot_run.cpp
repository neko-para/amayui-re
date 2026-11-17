// emulator-lift · Shape A / T2b：**AOT 执行循环**（`AotHost::run` + 块边界的宿主泵）
//
// ★ 复刻范围 = T2 §1.4 的**五件 guest 语义**（⛔ 不复刻 profiler / token / trace / watch / 块计时）：
//   ① `eip → 派发表项`（lifted）、`eip → lift_shim`（shim 桩）、两者都不是 ⇒ **响亮停**；
//   ② 三个合成 EA（`0x7FFEF008` 续延 / `0x7FFEF004` 定时器返回 / SEH 派发）+ 块边界定时器触发；
//   ③ 按**与解释器同一张表**（T2 §2.2）归类停因；
//   ④ `insns` 由 `lift_tick` 记账（`X86::insns`，与解释器同一份语义；D2）；
//   ⑤ 时钟 = **wall clock**（D2b：`clock_.setRealTime(true)`；⛔ 不引入虚拟时钟档）。
//   ★ 另有两条**默认档就改写 guest 状态**的 EA 钩子（T2 §1.2 #14）：`0x4BA718` 把 EAX 置 0
//     （`--accept-recovery-dialog` 的默认"绕过"）—— 不实现它 boot 会弹 MessageBoxA 并挂住。
//
// ★ 与解释器的**已知偏差**（都记在这里，⛔ 不装作没有）：
//   · `--present` 只有 headless（presenter 没有 sink，`gpuRaster()` = nullptr）；
//   · 没有 profiler / 影子栈 / guest token / 输入脚本 / 交互图 / 声音语义 —— 它们默认全关，
//     本来就是宿主控制面（T2 §7.4）；
//   · 边界→`cpu.eip` 的"块缓存"这一层不存在：EA 要么在派发表里、要么是 shim 桩，否则响亮停
//     （D10 P4：⛔ 不设"返回哨兵 = 结束"的约定）。
// ★ 本文件的诊断开关读 getenv（`LIFT_AOT_SHIM_TRACE`）；MSVC 口径里那是 deprecated 警告，不是问题。
#define _CRT_SECURE_NO_WARNINGS
#include "aot/aot_host.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "aot/aot_dispatch.h"
#include "runtime/control.h"  // Control（PIMPL 门面：⛔ 不含 beast / nlohmann / guest 头）
#include "runtime/guest_token.h"  // Phase-0 的 `arm()`（与解释器 `Machine::run` 同一个单例）

namespace lift {
namespace aot {
namespace {

// ★ `--accept-recovery-dialog` 的默认"绕过"判定点（真源 `machine.cpp:47` 的 `kRecoveryCheckEa`）。
//   它是 `sub_454730` 的返回地址（块起点）：EAX = "$$AGE.TMP 能不能打开" ⇒ 置 0 = "打不开" = 上次
//   正常结束 ⇒ `cmp eax,1` 为假 ⇒ 走 0x4BA856 干净返回，**永不到 0x4BA825 的 MessageBoxA**。
//   ★ 常量在 `aot/aot_hook_ea.h`（**唯一一份**：S7 之后生成器也读它，保证这个 EA 留在派发边界上）。
//   ★ 这个常量**只对 D1 pin 住的那一份 exe** 成立 —— 换 exe 会在 `loadImage` 就被 pin 拦下。

// 每 4096 条查一次墙钟（与解释器 `machine.cpp:1148-1157` 同粒度）。
constexpr std::uint64_t kCheckStride = 4096u;

// ★ S3：音频泵的节流粒度**必须**与块边界的 `kCheckStride` 同一份（`AudioPumpGate::kStride`）。
//   两处各写各的 = 迟早漂，而漂了**没有任何症状**（只是 fill 变稀 / 变密）。这条 `static_assert`
//   是唯一会响的地方 ⇒ ⛔ 不要删它去"绕过"编译错，改其中一处即可。
static_assert(kCheckStride == AudioPumpGate::kStride,
              "音频泵的节流粒度（AudioPumpGate::kStride）必须 == 块边界的 kCheckStride");

// ★ S7（D53）：生成代码的 `lift_dyn` 用的派发表查找 —— 与 trampoline 走的是**同一张表**
//   （同一个 `aot_dispatch.cpp` 的哈希槽数组，⛔ 不另建第二份），但**不记 trampoline 的派发账**：
//   那是"尾调用进目标"的一次查找，不是一次派发（见 `lookupUncounted` 的注释）。
LiftFn lookupForLiftDyn(std::uint32_t ea) { return lookupUncounted(ea); }

// ★ 定时器泵的"没有到期项"哨兵（见下面 ⑤ 的节流）：`due_100ns` 是不可能取到的最大值。
constexpr std::uint64_t kNoTimerDue = ~std::uint64_t{0};

// ★ 诊断开关（**默认关、零成本**；存在即开，与 `LIFT_FRAME_TRACE` 同口径）：
//   `LIFT_AOT_SHIM_TRACE=1` ⇒ 每次走 `lift_shim` 打一行 `[aot-shim] …`（module!name / 桩 EA / insns）。
//   为什么需要它：与解释器臂的 `--trace-shims` **同一种行**，两条臂的 shim 序列可以直接对齐
//   ⇒ "在哪一次导入调用上分叉"是**读数**，不是猜。
bool shimTraceOn() {
    static const bool on = std::getenv("LIFT_AOT_SHIM_TRACE") != nullptr;
    return on;
}

}  // namespace

// =====================================================================================
// 停因名字 / ok 口径（对照 `machine.h:46-82` 的 `StopReason` + `machine.cpp:82-107` 的名字表）
// =====================================================================================
const char* aotStopName(AotStop r) {
    switch (r) {
        case AotStop::None: return "none";
        case AotStop::Stopped: return "stopped";
        case AotStop::MaxInsns: return "max-insns";
        case AotStop::UntilEa: return "until-ea";
        case AotStop::TimeLimit: return "time-limit";
        case AotStop::Unimplemented: return "unimplemented";
        case AotStop::DecodeError: return "decode-error";
        case AotStop::NoCode: return "no-code";
        case AotStop::ResourceExhausted: return "resource-exhausted";
        case AotStop::Trap: return "unimplemented-slot";
        case AotStop::UnhandledException: return "unhandled-exception";
        case AotStop::NonContinuableException: return "noncontinuable-exception";
    }
    return "?";
}

bool aotStopOk(AotStop r) {
    // 与 `machine.h:147-149` 的 `RunResult::ok()` 同一组（本臂没有 FirstShim）。
    return r == AotStop::UntilEa || r == AotStop::MaxInsns || r == AotStop::Stopped ||
           r == AotStop::TimeLimit;
}

AotStop AotHost::hostStopReason(const X86& cpu) {
    // 判据的真源是 `machine.cpp:75-80`（那份在匿名 namespace 里，跨 TU 拿不到）⇒ 抄**判据**：
    // ① ResourceExhausted = 我方 arena / guest 堆不够；② Trap = 已注册的槽 / 形态没实现；
    // ③ 其余（None）= 实现里主动停 ⇒ Stopped（**正常结局**）。
    if (cpu.stop_kind == HostStopKind::ResourceExhausted) return AotStop::ResourceExhausted;
    if (cpu.stop_kind == HostStopKind::Trap) return AotStop::Trap;
    return AotStop::Stopped;
}

void AotHost::syncCpuFromRegs() { liftRegsToCpu(regs_, cpu_); }

void AotHost::syncRegsFromCpu() { liftCpuToRegs(cpu_, regs_); }

// =====================================================================================
// 宿主置停的归类（shim 路径）
// =====================================================================================
void AotHost::classifyShimStop(std::uint32_t def_id_plus_one, AotRunResult& r) {
    // ★ 真源 `machine.cpp:1651-1675`。三种"停"要分清，顺序固定：
    //   ① 注册表里没有 fn（或桩 EA 根本不在注册表里）= 缺口（Unimplemented，红）；
    //   ② 有 fn + 宿主停机种类非 None = 不是正常结局（Trap / ResourceExhausted）；
    //   ③ 有 fn + 种类 None = 实现里主动停（ExitProcess / --max-frames / 影片放完 …）⇒ Stopped（正常）。
    //   ⚠ `had_impl` 对 ② 与 ③ **都是真** —— 这就是要 `X86::stop_kind` 的原因。
    //   ★ 按 **id** 重新解析（⛔ 不用调用前那个指针：`lift_shim` 里可能 addRuntime 扩容）。
    const ShimDef* def = def_id_plus_one != 0u ? shims_.byId(def_id_plus_one - 1u) : nullptr;
    if (def == nullptr) {
        // `lift_shim` 在"桩 EA 不在注册表"时会走 `trapUnimplemented`（置 Trap + stop_why）。
        r.reason = hostStopReason(cpu_);
        r.detail = cpu_.stop_why != nullptr
                       ? std::string(cpu_.stop_why) + "（桩 EA 不在 shim 注册表里）"
                       : std::string("桩 EA 不在 shim 注册表里（stop_why 为空）");
        return;
    }
    const bool had_impl = def->fn != nullptr;
    if (!had_impl) {
        r.reason = AotStop::Unimplemented;
        r.detail = std::string("未实现的导入 ") + def->module + "!" + def->name;
        return;
    }
    r.reason = hostStopReason(cpu_);
    r.detail = cpu_.stop_why != nullptr
                   ? std::string(cpu_.stop_why) + "（shim " + def->module + "!" + def->name + "）"
                   : std::string("shim 主动结束运行：") + def->module + "!" + def->name;
}

// =====================================================================================
// 块边界的三个合成 EA
// =====================================================================================
AotHost::Pump AotHost::pumpSyntheticEas(AotRunResult& r) {
    // ---- ① 嵌套 guest 回调的续延点（`guest_call.h`；真源 `machine.cpp:1301-1321`）----
    if (cpu_.eip == GuestCallStack::kContinuationEa) {
        std::string call_err;
        ++callResumes_;
        if (!calls_.resume(cpu_, &call_err)) {
            r.reason = AotStop::Unimplemented;
            r.detail = call_err.empty() ? std::string("guest 回调续延失败") : call_err;
            return Pump::Stop;
        }
        // 音频泵借用 `[esp]` 作为 `sub_4B6C30` 的返回点；续延完成后必须还回原返回地址。
        // `--audio off` 下恒等，但顺序与解释器一致（`machine.cpp:1309`）。
        audio_.afterGuestCall();
        // 续延里也可能**主动结束运行**（例如影片停机链）：那时 `while (running)` 会直接退出，
        // 不给原因就成了静默缝 ⇒ 这里补 StopReason::Stopped（与解释器同口径）。
        if (!cpu_.running) {
            r.reason = AotStop::Stopped;
            r.detail = cpu_.stop_why != nullptr ? cpu_.stop_why : "guest 调用续延里主动结束运行";
            return Pump::Stop;
        }
        return Pump::Handled;
    }

    // ---- ② winmm 定时器回调的返回点（`x86.h:98`；真源 `machine.cpp:1322-1345`）----
    if (cpu_.eip == kTimerCallbackReturnEa) {
        if (!cpu_.shim.timer_firing) {
            r.reason = AotStop::Unimplemented;
            r.detail = "定时器续延点被调到，但没有正在跑的定时器回调";
            return Pump::Stop;
        }
        cpu_.esp() = cpu_.shim.timer_saved_esp;
        cpu_.eip = cpu_.shim.timer_resume_ea;
        // ★ 整份通用寄存器都要还回去：真机上回调在另一个线程，被打断线程的寄存器组由 OS 保住。
        //   只还 EIP/ESP 的话，回调随手改掉的 EAX/ECX/EDX 会漏进被插入点的后续指令。
        const TimerSavedRegs& sr = cpu_.shim.timer_saved_regs;
        cpu_.eax() = sr.eax;
        cpu_.ebx() = sr.ebx;
        cpu_.ecx() = sr.ecx;
        cpu_.edx() = sr.edx;
        cpu_.esi() = sr.esi;
        cpu_.edi() = sr.edi;
        cpu_.ebp() = sr.ebp;
        cpu_.eflags = sr.eflags;
        cpu_.shim.timer_firing = false;
        return Pump::Handled;
    }

    // ---- ③ SEH / C++ EH 派发（**只在块边界推进**；真源 `machine.cpp:1346-1355`）----
    if (seh_.busy()) return pumpSeh(r);

    return Pump::NotHandled;
}

AotHost::Pump AotHost::pumpSeh(AotRunResult& r) {
    // ★ 拆自 `Machine::pumpSeh`（`machine.cpp:2208-2280`）；去掉的部分只有影子栈（profiler）。
    //   handler 的合成返回点（`kSehHandlerReturnEa`）不是可解码地址 ⇒ 本泵必须在"派发表查找"之前，
    //   否则"异常没人接"会被误报成"EA 不在表里"。
    if (cpu_.eip == kSehDispatchEa) {
        seh_.setDispatchPending(false);
        // ★ "展开不许跨过宿主 guest 回调帧"的判据（真机上是 UB）。0 = 不在任何回调里。
        seh_guest_boundary_ = 0;
        const std::uint32_t stack_top = setup_.stack_top != 0 ? setup_.stack_top : layout_.stack_top;
        if (calls_.active()) {
            // 从当前 esp 往上找最内层回调帧：`GuestCallStack::start()` 在那一格写 `kContinuationEa`。
            for (std::uint32_t ea = cpu_.esp(); ea + 4u <= stack_top; ea += 4u) {
                if (mem_.rd32(ea) == GuestCallStack::kContinuationEa) {
                    seh_guest_boundary_ = ea;
                    break;
                }
            }
            if (seh_guest_boundary_ == 0) {
                r.reason = AotStop::UnhandledException;
                r.detail = std::string("guest 异常发生在 GuestCallStack 回调里，但栈上找不到回调帧的"
                                       "返回槽（") +
                           (calls_.currentApi() ? calls_.currentApi() : "?") +
                           "）⇒ 无法证明展开不跨宿主帧 ⇒ 本线不支持";
                seh_.reset();
                cpu_.running = false;
                return Pump::Stop;
            }
        } else if (cpu_.shim.timer_firing) {
            // `fireDueTimer` 的帧：`[timer_saved_esp-24] = kTimerCallbackReturnEa`（5 参 + 返回地址）。
            seh_guest_boundary_ = cpu_.shim.timer_saved_esp - 24u;
        }
    }
    std::string why;
    const SehStep s = seh_.step(cpu_, mem_, &why);
    switch (s.kind) {
        case SehStep::Kind::None:
            return Pump::NotHandled;
        case SehStep::Kind::CallHandler:
            // 合成 handler 调用也是一帧（它的 `ret N` 会正常弹）。`step` 已写好 eip / esp。
            cpu_.noteInsn(s.handler_ea, "[seh]");
            return Pump::Handled;
        case SehStep::Kind::Transfer:
            // ★ 跨宿主 guest 回调帧 ⇒ 响亮停（真机上异常会穿过宿主 C++ 栈 = UB）。
            if (seh_guest_boundary_ != 0 && s.esp >= seh_guest_boundary_) {
                char buf[320];
                std::snprintf(buf, sizeof(buf),
                              "guest 异常要展开跨过宿主 guest 回调帧（落地 esp=0x%08X >= 回调帧返回槽 "
                              "0x%08X；%s）⇒ 本线不支持（⛔ 不静默清栈继续跑）",
                              s.esp, seh_guest_boundary_,
                              calls_.active() ? "GuestCallStack" : "winmm 定时器回调");
                r.reason = AotStop::UnhandledException;
                r.detail = buf;
                seh_.reset();
                seh_guest_boundary_ = 0;
                cpu_.running = false;
                return Pump::Stop;
            }
            cpu_.noteInsn(s.eip, "[seh]");
            return Pump::Handled;
        case SehStep::Kind::Stop:
            r.reason = s.noncontinuable_violation ? AotStop::NonContinuableException
                                                  : AotStop::UnhandledException;
            r.detail = why.empty() ? std::string("SEH 派发失败（无 detail）") : why;
            seh_guest_boundary_ = 0;
            cpu_.running = false;
            return Pump::Stop;
    }
    return Pump::NotHandled;
}

bool AotHost::fireDueTimer(AotRunResult& r) {
    // ★ 拆自 `Machine::fireDueTimer`（`machine.cpp:2140-2199`）：协作式定时器，**只在块边界**触发。
    //   到期判定用 `clock_`（D2b：wall clock）⇒ 与解释器同一份时钟语义。
    const std::uint64_t now = clock_.now100ns();
    std::vector<GuestTimer>& timers = cpu_.shim.timers;
    std::size_t best = timers.size();
    for (std::size_t i = 0; i < timers.size(); ++i) {
        if (timers[i].due_100ns > now) continue;
        // 确定性：同刻到期时取 id 最小的（⛔ 不依赖 vector 的插入顺序）
        if (best == timers.size() || timers[i].due_100ns < timers[best].due_100ns ||
            (timers[i].due_100ns == timers[best].due_100ns && timers[i].id < timers[best].id)) {
            best = i;
        }
    }
    if (best == timers.size()) return false;

    const GuestTimer t = timers[best];
    if (t.oneshot) {
        timers.erase(timers.begin() + static_cast<std::ptrdiff_t>(best));
    } else {
        // 周期定时器先续约（回调里 timeKillEvent 仍能按 id 杀掉它）
        timers[best].due_100ns += static_cast<std::uint64_t>(t.period_ms) * 10000ull;
    }
    if (t.callback == 0) {
        // ★ 节流到第一次（照 `present.cpp:43` 的 `[present] sink 失败`）：周期定时器的回调为 NULL 时
        //   每次到期都会走到这里 ⇒ 不节流就是"一行/到期"。丢弃语义不变（⛔ 只是不再刷屏）。
        if (!timer_null_warned_) {
            timer_null_warned_ = true;
            std::fprintf(stderr, "[aot] 定时器 %u 的回调为 NULL ⇒ 丢弃\n", t.id);
        }
        return false;
    }
    // 回调帧：TimeProc(uID, uMsg, dwUser, dw1, dw2)（stdcall，5 个参数 = 20 字节 + 返回地址）
    constexpr std::uint32_t kFrame = 4u + 5u * 4u;
    const std::uint32_t base = cpu_.esp();
    if (base < kFrame || !mem_.committed(base - kFrame, kFrame)) {
        r.reason = AotStop::Unimplemented;
        r.detail = "guest 栈不够铺定时器回调帧（esp=" + std::to_string(base) + "）";
        return false;
    }
    cpu_.shim.timer_resume_ea = cpu_.eip;
    cpu_.shim.timer_saved_esp = base;
    cpu_.shim.timer_firing = true;
    cpu_.shim.timer_firing_id = t.id;
    {
        TimerSavedRegs& sr = cpu_.shim.timer_saved_regs;
        sr.eax = cpu_.eax();
        sr.ebx = cpu_.ebx();
        sr.ecx = cpu_.ecx();
        sr.edx = cpu_.edx();
        sr.esi = cpu_.esi();
        sr.edi = cpu_.edi();
        sr.ebp = cpu_.ebp();
        sr.eflags = cpu_.eflags;
    }
    cpu_.esp() = base - kFrame;
    mem_.wr32(base - 24u, kTimerCallbackReturnEa);
    mem_.wr32(base - 20u, t.id);
    mem_.wr32(base - 16u, 0u);  // uMsg（文档：保留，不用）
    mem_.wr32(base - 12u, t.user);
    mem_.wr32(base - 8u, 0u);   // dw1（保留）
    mem_.wr32(base - 4u, 0u);   // dw2（保留）
    cpu_.eip = t.callback;
    ++timerFires_;
    return true;
}

// =====================================================================================
// 执行循环
// =====================================================================================
AotRunResult AotHost::run(const AotRunOptions& opts) {
    AotRunResult r;
    // ★ S7（D53）：给生成代码的 `lift_dyn` 装上**唯一的派发表查找**（`aot/aot_dispatch.h` 的
    //   `lookup` = 与 trampoline 同一张表）⇒ 间接 `jmp` / `call reg|mem` 的尾调用能路由到目标。
    //   缺省 `nullptr` 时 `lift_dyn` 会**响亮停**（⛔ 不静默）；这里装上之后才允许跑生成代码。
    lift::liftDynLookup = &lookupForLiftDyn;
    if (!prepared_) {
        r.reason = AotStop::NoCode;
        r.detail = "AotHost 还没 prepare（没有 guest 进程结构 / shim 桩）⇒ 不能执行";
        return r;
    }
    if (!generatedPresent()) {
        // ★ 响亮：**不许**把"没有生成物"静默跑成空壳（构建期缺 clang / 执行源 / closure.txt）。
        r.reason = AotStop::NoCode;
        r.detail = "没有生成物（构建期缺 clang / 执行源 / closure.txt）⇒ 派发表为空，无法执行任何 guest 指令";
        return r;
    }
    // ★ Phase-0 令牌读数（REQ-01M75WWEDC20GEH7QVSHPHH4GP）：与解释器 `machine.cpp:957` **同口径**，
    //   **每轮一次**。`arm()` 自己清上一轮的账，并把 `LIFT_AUDIO_TRACE` 或上 `opts.profile`（唯一读点）。
    //   ⇒ 缺省档（不给 `--profile`、不设该 env）`phase0_enabled_` 仍是 false：热路径只多一次 bool 读、
    //   不读时钟，摘要一行不加（默认档 stdout 逐字不变）。
    //   ★ 之前**只有** `Machine::run` 调它 ⇒ AOT 臂的 `tokenhold/holdhist/waithist/shimmax` 永远是 0/不打印；
    //     而音频 feeder 只在 AOT 上跑，那正是要量的地方。
    guestToken().arm(opts.profile);
    // ★ 派发表**预热**：音频真 feeder 线程（`--audio sdl` + 墙钟）会在自己的线程上查找派发表
    //   （`src/aot/aot_feeder.cpp`）⇒ 把"第一次摊表"钉在**起线程之前**，不依赖"主线程恰好先查过一次"
    //   的时序（摊表本身已 `std::call_once`，这里只是把 happens-before 提前）。⛔ 不写任何计数。
    ensureDispatchTable();

    // ★ **S2 生命周期 · begin**（与解释器 `machine.cpp:1056-1062` 同口径）：打开 SDL 设备 / dump 文件；
    //   失败**只降级，不阻止 boot**（与解释器一致：`begin` 失败只打一行 stderr）。
    //   ★ 位置 = 生成物检查**之后**：没有可执行的东西就不该占设备 / 建 WAV。
    //   ★ 缺省档（`--audio off`）时 `begin()` 立刻返回 true，不开设备、不建文件、不打任何输出
    //     ⇒ 默认档逐字不变。★ 与 `end()` **成对**：本函数从这里到最后只有一个出口。
    {
        std::string audio_err;
        if (!audio_.begin(&audio_err) && !audio_err.empty()) {
            std::fprintf(stderr, "[audio] begin 失败：%s\n", audio_err.c_str());
        }
    }

    // ---- 每轮开头（只留 guest 语义那几条；真源 `machine.cpp:943-1094`）----
    windows_.setOwnerThread(std::this_thread::get_id());
    seh_.reset();  // ★ 状态机**跨块存活、只跨 run 清**（machine.cpp:1007）
    calls_.reset();  // 与 seh 同口径：一次 run 的嵌套回调状态不跨 run
    cpu_.running = true;
    cpu_.stop_kind = HostStopKind::None;  // ★ 按轮重置（machine.cpp:1094）
    control_stop_ = false;               // 控制层的置停标志也是**按轮**的事实（不跨 run）
    cpu_.shim.max_frames = opts.max_frames;
    if (!clock_.realTime()) clock_.setRealTime(true);  // D2b：wall clock（⛔ 没有 --clock= 开关）
    resetDispatchStats();
    timerFires_ = 0;
    callResumes_ = 0;
    timer_null_warned_ = false;  // 节流按轮重置（一次 run 最多一行）
    // ★ `--profile`（缺省关）：清空上一轮的账 + **启动时**跑一次已知长度的忙循环校准机器频率。
    //   ⛔ 关档时这一行都不执行（`beginProfile` 不被调用 ⇒ `prof_.on == false` ⇒ 采样点全是死代码分支）。
    //   ★ 代价：开档时多花 ~60 ms（校准忙循环）；它发生在 `wall_start` 之前，**不进** `run()` 的墙钟，
    //     但 `frame-ms` 的 `t_ms` 是"自进程启动" ⇒ 开档的 `t_ms` 含这 ~60 ms（读数时记住这一点）。
    if (opts.profile) {
        beginProfile();
    } else {
        // ★ 关档必须是**真的**关：摘掉 `lift_one` 的采样钩子（上一轮开过也一样）。
        stopProfile();
    }
    // ★ 窗口 profile：**按轮重置**。窗口只由配方的 `profile_begin` 开（⛔ 不跨 run、⛔ 不自动开），
    //   `lift_dyn` 的计数闸门同步关掉 —— 缺省档在 `lift_dyn` 里只付一次 bool 判断。
    win_.on = false;
    ::liftDynCountOn = false;
    // ★ `--skip-logo`：一次性 latch 的**按轮**状态（语义 = `AotRunOptions::skip_logo`）。
    //   解释器那四个成员是 Machine 的（`machine.h:555-558`，进程只跑一轮）；本宿主的 `run()` 可以
    //   被调多次 ⇒ 在这里按轮重置，保证"同一台宿主第二轮不会假装命中过"。
    skip_logo_ = opts.skip_logo;
    skip_logo_checked_ = false;
    skip_logo_applied_ = false;
    skip_logo_object_ = 0;
    skip_logo_prior_value_ = 0;
    // ★ 逐帧 `lastHash` 读数（D）：**opt-in**（`--frame-log`；`--profile` 档顺带保留 ⇒ 存档逐字不变）。
    //   Present 只可能由**shim** 触发（生成物里没有 inline 的 `lift_shim` 调用；
    //   `rg lift_shim src/aot/lift_gen_emit.cpp` = 0）⇒ 打点在下面 shim 分支里、`lift_shim` 返回之后
    //   （在 `!cpu_.running` 之前 —— `--max-frames` 恰好在 Present 里置停时那一帧不能丢）。
    //   ★ 为什么缺省关：行数 = 帧数（300 帧 ⇒ 300 行，占默认 stdout 的 78%），而它只是**画面序列**读数；
    //     末帧 hash 另有出口（摘要 `present : … lastHash=0x…`）。
    const bool frame_log_on = opts.frame_log || prof_.on;  // ★ `|| prof_.on`：已有 profile 存档的可比面
    std::uint64_t logged_frames = presenter_.frameCount();
    auto logPresentedFrames = [&]() {
        const std::uint64_t n = presenter_.frameCount();
        if (n == logged_frames) return;
        // ★ 窗口 profile 的 `present` 子桶（⊂ shim；**零额外成本** —— `presentBreakdown()` 本来
        //   就一直在计时）。⚠ 一次 shim 里多次 Present 时只有最后一帧的拆分（只增计数）。
        if (win_.on) win_.notePresent(presentBreakdown(), n - logged_frames);
        if (n == logged_frames + 1) {
            if (frame_log_on) {
                std::printf("[frame] n=%llu hash=0x%016llX\n", static_cast<unsigned long long>(n),
                            static_cast<unsigned long long>(presenter_.lastHash()));
            }
            // ★ `--profile` 的**每帧时间戳**（1 行/帧，开销 = 一次 `GetSystemTimeAsFileTime`）。
            //   `t_ms` = 自**进程启动**到该帧 present 完成的毫秒（Windows 用 `GetProcessTimes` 的
            //   creation 时间当基准，⛔ 不用"程序跑到这里"近似）；`insns` = 当时的 `cpu_.insns`。
            if (prof_.on) {
                std::printf("frame-ms: n=%llu t_ms=%.3f insns=%llu\n", static_cast<unsigned long long>(n),
                            profileProcessMs(), static_cast<unsigned long long>(cpu_.insns));
                ++prof_.frame_lines;
            }
        } else {
            // 一次 shim 里发生了多次 Present（正常是 1 次）：hash 只有最后一帧那一份，⛔ 不冒充逐帧。
            if (frame_log_on) {
                std::printf("[frame] n=%llu..%llu（一次 shim 里 %llu 次 Present）hash=0x%016llX（只有最后一帧）\n",
                            static_cast<unsigned long long>(logged_frames + 1),
                            static_cast<unsigned long long>(n),
                            static_cast<unsigned long long>(n - logged_frames),
                            static_cast<unsigned long long>(presenter_.lastHash()));
            }
            if (prof_.on) {
                // 这一格里发生了多次 Present ⇒ 只有一个时刻（它们同一趟 shim 里跑完的）。
                // ⛔ 不伪造 K 个时间戳：那会变成"看起来逐帧、其实只有一次取样"。
                std::printf(
                    "frame-ms: n=%llu..%llu t_ms=%.3f insns=%llu（一次 shim 里 %llu 次 Present：只有最后"
                    "一个时刻）\n",
                    static_cast<unsigned long long>(logged_frames + 1),
                    static_cast<unsigned long long>(n), profileProcessMs(),
                    static_cast<unsigned long long>(cpu_.insns),
                    static_cast<unsigned long long>(n - logged_frames));
                ++prof_.frame_lines;
            }
        }
        logged_frames = n;
    };
    // ★ 控制层 **Present 闸门**（S2）：装一次，`present()` 在 sink / dump 之后回调它。
    //   位置只在 `present.cpp`（⛔ 不在这里另找时机）；没接控制层时不装 ⇒ `present()` 里一次判空。
    if (control_ != nullptr) {
        presenter_.setPresentGate(&AotHost::presentGateThunk, this);
        // ★ **冻结最后呈现帧的像素副本**（`REQ-01M7JAGEKMYWWQPMWGV01Y1FNQ`）：唯一产品调用点，
        //   与 `setPresentGate` **同处同条件**（`control_ != nullptr`）⇒ 缺省档零成本。
        //   ⛔ 不要派生自 `hasPresentGate()`（那样任何新闸门都会静默背上 3.7 MB/帧）；
        //   ⛔ `--dump-frame` 不走副本（它是"当场写"的独立对照）。
        presenter_.setFreezeFrame(true);
    }
    // ★ 起点：`cpu_.eip` 已经是 PE entry（`prepare` 设的）⇒ lifted 侧现场由它派生。
    syncRegsFromCpu();
    const auto wall_start = std::chrono::steady_clock::now();
    std::uint64_t next_time_check = 0;
    // ★ 块边界事件泵的节流（与解释器 `machine.cpp:1097` / `:1298` 同粒度：每 4096 条一次）。
    std::uint64_t next_present_check = 0;
    // ★ 控制层**重停谓词**（`resume --for-*`）的轮询节流（与上面同一个 kCheckStride 口径）：
    //   `watching()` 是一次 atomic load；只有真的武装了谓词才读 `steady_clock` / `frameCount`。
    std::uint64_t next_control_check = 0;
    // ★ 定时器泵的节流（S2）：`steady_clock::now()` 从"每轮派发"降到"每 kCheckStride 条一次"。
    //   两次采样之间只做一次**整数比较**（缓存的"最早到期时间" vs 缓存的"上次采样时刻"）⇒
    //   绝大多数轮次连取时都不做。语义口径不变（D2b）：判定用 `clock_` 的同一份墙钟、同刻到期
    //   取 id 最小者（那是 `fireDueTimer` 自己的选择逻辑，未改）、oneshot 仍移除、周期仍续约、
    //   回调帧仍逐字节同样铺。唯一差别 = **到期判定的粒度是 4096 条 guest 指令**（与 `--time-limit`
    //   同一个口径）；本轮 300 帧里 `synthetic: 定时器回调=0` ⇒ 对读数逐位无影响（A/B 实测见报告）。
    std::uint64_t next_timer_check = 0;
    std::uint64_t timer_due_hint = kNoTimerDue;  // 上次采样时 `timers` 里最早的 due_100ns
    std::uint64_t timer_now_hint = 0;            // 上次采样的 `clock_.now100ns()`（同一份时钟）
    // ★ 桩页判据的**一次减法**：取注册表自己的 page/slots（与 `byStubEa` 同一个来源，⛔ 不取 layout_）。
    const std::uint32_t stub_page = shims_.stubPage();
    const std::uint32_t stub_span = shims_.stubSlots() * 4u;
    // 定时器泵的"最早到期时间"（只供上面的节流用；O(timers) 且**只在采样点/触发后**调用）。
    auto earliestTimerDue = [&]() -> std::uint64_t {
        std::uint64_t due = kNoTimerDue;
        for (const GuestTimer& t : cpu_.shim.timers) {
            if (t.due_100ns < due) due = t.due_100ns;
        }
        return due;
    };
    std::uint32_t ea = cpu_.eip;

    // ★ `--profile` ②：**派发归因**（自计时间隔法 / self-time）。
    //   每 `kDispatchStride` 次派发取一次 TSC，把"距上次采样之间消耗的 ticks"整笔记到
    //   **这次采样的入口 EA** 上。⛔ 不逐次计时（两次取时就与一个块同量级，仪器会喧宾夺主）；
    //   开档的逐次成本只有一条 `++` 与一次比较。
    //   ★ 第一个样本只立基线：它没有"上一个采样点"，强行记会把启动前的未知区间算进去。
    auto profileDispatchSample = [&](std::uint32_t at_ea) {
        if (!prof_.on) return;
        ++prof_.dispatch_calls;
        if (++prof_.dispatch_seen < AotProfile::kDispatchStride) return;
        prof_.dispatch_seen = 0;
        const std::uint64_t now = profileTicks();
        if (prof_.dispatch_last == 0) {
            prof_.dispatch_last = now;
            return;
        }
        const std::uint64_t delta = now - prof_.dispatch_last;
        prof_.dispatch_last = now;
        prof_.dispatch_total += delta;
        ++prof_.dispatch_samples;
        auto& slot = prof_.dispatch_by_ea[at_ea];
        ++slot.first;
        slot.second += delta;
    };

    for (;;) {
        // ★ 窗口 profile 的**第一个**取时点（`win_.on` 为假时这一步只付一次 bool 读）：
        //   `t_a` = 块顶（= boundary 桶的起点），`t_b` 在分派前取（见 ⑥ 之前）。
        //   只有被选中的迭代取时（小窗口全量 / 大窗口 1/64）⇒ 成本 ~1 ns/派发。
        std::uint64_t win_a = 0;
        std::uint64_t win_b = 0;
        bool win_sample = false;
        if (win_.on) {
            ++win_.iter;
            win_sample = win_.sampleDue();
            if (win_sample) {
                win_a = windowNowNs();
                ++win_.sampled;
            }
        }
        // ★ 块边界的第一件事：把 lifted 侧的寄存器现场同步进 `cpu_`（宿主侧逻辑读 cpu）。
        //   ⛔ 反方向的同步只在宿主侧真的改过 cpu 之后做（下面 Pump::Handled 的三处）。
        syncCpuFromRegs();
        cpu_.eip = ea;

        // ---- ⓪− 控制层闸门（`--control-root`；缺省 nullptr ⇒ 只有一次判空）----
        //   ★ 位置 = 寄存器同步之后、合成 EA 泵之前（`design-review.md` §1 第 2 条的裁决）：
        //     此刻 `cpu_` 已与 lifted 侧一致、`ea` = 本轮块入口、没有 in-flight shim。
        //   ★ **只读语义**：`serve()` 只动宿主对象（时钟冻结 / `postQuit` / 置停标志），
        //     ⛔ 不写 `cpu_` / `regs_` ⇒ 返回后**不需要**反向同步；暂停对 guest 不可见。
        //   ★ 返回 true = 已置停（客户端 `quit` / idle / orphan）⇒ 立即收口，⛔ 不再进下一块。
        if (control_ != nullptr &&
            (control_->pending() || (control_->watching() && cpu_.insns >= next_control_check))) {
            Control::Context gate_ctx;
            gate_ctx.clock = &clock_;
            gate_ctx.windows = &windows_;
            gate_ctx.stop_requested = &control_stop_;
            gate_ctx.paused_at = "block";
            gate_ctx.host = this;
            if (control_->serve(gate_ctx)) {
                r.reason = AotStop::Stopped;
                r.detail = "control: quit";
                cpu_.running = false;
                break;
            }
            next_control_check = cpu_.insns + kCheckStride;
        }
        // ★ 置停可能发生在**上一个块边界之后**（Present 闸门里的 `quit` / idle / orphan）：块边界
        //   读一次 bool（`control_ == nullptr` 时连这一读都不付）⇒ 立即收口，⛔ 不再进下一块。
        if (control_ != nullptr && control_stop_) {
            r.reason = AotStop::Stopped;
            r.detail = "control: quit";
            cpu_.running = false;
            break;
        }

        // ---- ⓪ 外部停止请求（`--input-plan` 的 `quit`；`AotRunOptions::stop_requested`）----
        //   ★ 与解释器臂**同一条口径**（`machine.cpp:1270`：`actuator().quitRequested()` ⇒
        //     `Stopped` + detail `… quit`）：本次运行在**下一个块边界**收口，⛔ 不等 guest 自己退出。
        //     不等它的理由：guest 的停机路径可能不在闭包里（实测 WM_QUIT 之后停在 `00513B90`
        //     ⇒ `decode-error`，退出码 2），那与"quit 是正常结局"这条语义冲突。
        //   ★ 判据只读一个 bool（读点在主线程写、这里主线程读）⇒ 缺省 nullptr 时是一条判空。
        if (opts.stop_requested != nullptr && *opts.stop_requested) {
            r.reason = AotStop::Stopped;
            r.detail = "input-plan: quit";
            cpu_.running = false;
            break;
        }

        // ---- ① `--time-limit`（每 4096 条查一次墙钟；machine.cpp:1148-1157）----
        if (opts.time_limit_sec > 0.0 && cpu_.insns >= next_time_check) {
            const auto now = std::chrono::steady_clock::now();
            // ★ 暂停不是运行时间（父级 2026-11-16 裁决 / 判据 6）：`--time-limit` 与 guest 时钟同口径
            //   扣掉累计暂停。没有暂停过 ⇒ `pausedTotalMs() == 0` ⇒ 与改动前逐位等价。
            double elapsed = std::chrono::duration<double>(now - wall_start).count();
            elapsed -= static_cast<double>(clock_.pausedTotalMs()) / 1000.0;
            if (elapsed >= opts.time_limit_sec) {
                r.reason = AotStop::TimeLimit;
                r.detail = "达到 --time-limit（墙钟上限，已扣暂停）";
                break;
            }
            next_time_check = cpu_.insns + kCheckStride;
        }

        // ---- ①' 显示后端：块边界也要泵（真源 `machine.cpp:1279-1299`）----
        //   ⛔ 不泵的后果是**窗口假死 / 关窗不停**：boot→TITLE 有单帧 ~7.3 亿条指令，而 Present
        //   内部的泵只在两帧之间跑 ⇒ 那一段窗口不响应，`stopRequested()` 也没人问。
        //   ⛔ 嵌套 guest 调用（或 SEH 派发）进行中不泵：那一刻正在跑另一段 guest 代码，插一个
        //   WndProc 帧会改坏它仍活着的上下文（真机上它们是两个线程）。
        {
            const bool nested_guest_active = calls_.active() || seh_.busy();
            if (!nested_guest_active && presenter_.hasSink() && cpu_.insns >= next_present_check) {
                presenter_.pumpHostEvents();
                if (presenter_.stopRequested()) {
                    r.reason = AotStop::Stopped;
                    // ★ 控制层口径（S4）：暂停期间的关窗 / ESC ⇒ `bye.reason:"window-closed"`，
                    //   ⛔ 不投 `WM_CLOSE` 进 guest、⛔ 不把"停止"和"谁请求的"压进一个枚举。
                    const char* host_reason = presenter_.controlStopReason();
                    if (host_reason != nullptr) {
                        r.host_stop_reason = host_reason;
                        r.detail = "control: 暂停期间的关窗 / ESC（宿主 stop）";
                    } else {
                        r.detail = "present: 窗口关闭";
                    }
                    cpu_.running = false;
                    break;
                }
                next_present_check = cpu_.insns + kCheckStride;
            }
        }

        // ---- ② 三个合成 EA（必须在"桩 EA ⇒ lift_shim"之前）----
        //   ★ 顺序的真正理由：它们**不在派发表里**（是宿主合成的返回点，不是镜像里的代码），
        //     晚于 ③ 的查表就会落 `DecodeError`（`aot_run.cpp:800-814` 的响亮停）。
        //   ⛔ 旧注释写的「它们都落在桩页范围内」是**假的**：`0x7FFEF004/008/010/014` 全在桩页
        //     上界 `0x7FFE8000` **之上**（桩页 = `[0x7FFE0000, 0x7FFE8000)`，`image.h:98-99`）；
        //     页内判定（`stub_off < stub_span`）对它们恒假 ⇒ 它们从来不是"桩 EA"。
        const Pump pumped = pumpSyntheticEas(r);
        if (pumped == Pump::Stop) break;
        if (pumped == Pump::Handled) {
            // 泵可能改了 esp / eip / 寄存器（铺帧 / 恢复现场 / SEH 转移）⇒ 反向同步一次。
            syncRegsFromCpu();
            ea = cpu_.eip;
            continue;
        }

        // ---- ③ `--until` / `--max-insns`（块边界；machine.cpp:1356-1363）----
        if (opts.until_ea != 0 && ea == opts.until_ea) {
            r.reason = AotStop::UntilEa;
            break;
        }
        if (opts.max_insns != 0 && cpu_.insns >= opts.max_insns) {
            r.reason = AotStop::MaxInsns;
            break;
        }

        // ---- ④ `--accept-recovery-dialog` 的默认"绕过"（EAX := 0；machine.cpp:1369-1376）----
        //   ★ 这是默认档就改写 guest 寄存器的钩子（T2 §1.2 #14）：不实现它 boot 会弹系统框并挂住。
        //   只应用一次；`regs_` 是寄存器真源 ⇒ 两个都写。
        if (!recovery_bypass_checked_ && ea == kRecoveryCheckEa) {
            recovery_bypass_checked_ = true;
            recovery_bypass_prior_eax_ = cpu_.eax();
            cpu_.eax() = 0;
            regs_.r[0] = 0;  // EAX 在 LiftRegs 的 +0（`lift_abi.h:28` 的 GprIndex 顺序）
            recovery_bypass_applied_ = true;
        }

        // ---- ④' `--skip-logo`：一次性 latch，与解释器**同一格内存**（真源 `machine.cpp:1377-1401`）----
        //   ★ 钩子点是 `0x004BAA33`，不是解释器的 `0x004BAA37`（理由见 `aot_host.h` 的常量注释：
        //     0x4BAA37 只是同函数内部块，派发表里没有它 ⇒ 挂那里**永不命中**）。
        //   ★ 此刻 `syncCpuFromRegs()`（循环头）已把 lifted 侧寄存器同步进 `cpu_` ⇒ `cpu_.eax()` 就是
        //     `sub_415640`（`T_Command` 构造）的返回值；`0x4BAA33` → `0x4BAA37` 之间只有一条
        //     `jmp short`（无语义）⇒ 在这里写 `+0x5EB5C` 与解释器在 `0x4BAA37` 写**逐位等价**，
        //     而且同样发生在 `0x4BAA3A` 的 `mov ds:dword_55E1BC, eax`（指针发布）之前。
        //   ★ 只写 **guest 内存**：不改 eip / 不改任何寄存器 ⇒ 不需要反向同步 `regs_`。
        //   ★ "初值不是 1" / "对象不可写"两条异常分支照解释器保留（⛔ 不静默）。
        if (skip_logo_ && !skip_logo_checked_ && ea == kSkipLogoHookEa) {
            skip_logo_checked_ = true;
            const std::uint32_t object = cpu_.eax();
            skip_logo_object_ = object;
            if (object != 0 && mem_.committed(object + kSkipLogoFieldOffset, 4)) {
                skip_logo_prior_value_ = mem_.rd32(object + kSkipLogoFieldOffset);
                mem_.wr32(object + kSkipLogoFieldOffset, 0);
                skip_logo_applied_ = true;
                // ★ latch 命中读数（A 的判据就是这一行）：命中 EA / 写入的 guest EA / 写入的值。
                std::printf("[skip-logo] hit @ %08X：T_Command=0x%08X ⇒ 写 [0x%08X] = 0（写前 %u）\n",
                            kSkipLogoHookEa, object, object + kSkipLogoFieldOffset, skip_logo_prior_value_);
                if (skip_logo_prior_value_ != 1) {
                    std::fprintf(stderr,
                                 "[skip-logo] ⚠ 初值不是 1（读到 %u）：写 0 仍执行，但钩子/字段可能已漂\n",
                                 skip_logo_prior_value_);
                }
            } else {
                std::fprintf(stderr,
                             "[skip-logo] 钩子 @ %08X 命中，但对象指针 0x%08X 不可写（+0x%05X）⇒ 未应用\n",
                             kSkipLogoHookEa, object, kSkipLogoFieldOffset);
            }
        }

        // ---- ⑤ 到期的 winmm 定时器在**块边界**触发（machine.cpp:1402-1418）----
        //   ⛔ 不嵌套：一个回调没跑完就不再触发下一个；嵌套 guest 调用 / SEH 期间也不触发。
        //   ★ S2 节流：`timers.empty()` 之外的判定用**缓存的比较量**，只在每 kCheckStride 条
        //     guest 指令时取一次 `clock_.now100ns()`（⛔ 不每轮派发取时；旧实现每轮一次 ≈ 21 ns）。
        //     `timer_due_hint <= timer_now_hint` ⇔ "上次采样时刻已经有定时器到期"
        //     ⇔ `fireDueTimer` 会选中一个（它的选择逻辑与判据**一个字都没改**）。
        if (!calls_.active() && !cpu_.shim.timer_firing && !cpu_.shim.timers.empty() &&
            !cpu_.shim.callback.active && !seh_.busy()) {
            if (cpu_.insns >= next_timer_check) {
                next_timer_check = cpu_.insns + kCheckStride;
                timer_now_hint = clock_.now100ns();
                timer_due_hint = earliestTimerDue();
            }
            if (timer_due_hint <= timer_now_hint) {  // ★ 绝大多数轮次：这一次整数比较就是全部开销
                if (fireDueTimer(r)) {
                    // 回调要跑很久（它自己也可能 timeSetEvent / timeKillEvent）⇒ 用**新的**采样
                    // 重算，⛔ 不拿旧 now 再判一次（否则周期定时器会按"旧时刻"空转重复触发）。
                    timer_now_hint = clock_.now100ns();
                    timer_due_hint = earliestTimerDue();
                    syncRegsFromCpu();
                    ea = cpu_.eip;
                    continue;
                }
                // `fireDueTimer` 报"没有到期的"（拿到更新的一次 now 之后）⇒ 把缓存刷新成事实。
                timer_due_hint = earliestTimerDue();
                if (r.reason != AotStop::None) break;
            }
        }

        // ---- ⑤' S3 音频泵（**块边界**；按 `kCheckStride` 节流，⛔ 不是每块）----
        //   ★ 为什么是**块边界**、而且正好在**这一格**（`aot_run.cpp:AotHost::run` 的 for 循环，
        //     ⑤ 定时器之后、⑥ 分派之前）：
        //     ① `pump()` 会**铺一个 guest 帧**（`sub_4B6C30`）⇒ 它只能在"宿主与 guest 之间"的
        //        边界上做。循环头刚 `syncCpuFromRegs()`（lifted 侧现场 → `cpu_`）⇒ 此刻
        //        `pump()` 里的 `capture()`（`audio.cpp`）拿到的是一份**自洽**的 guest 现场；
        //        若在派发中途做，`capture()` 会存下"半个块"的寄存器。
        //     ② 在 ⓪/①/②/⑤ 之后：那些步骤会 `continue`（续延点 / 定时器铺帧 / 控制层置停）⇒
        //        先让它们把 `cpu_.eip` 定下来，才不会在一个"其实是合成返回点"的边界上白泵一次。
        //     ③ 在 ⑥ 分派之前：解释器里同一件事也在分派之前（`machine.cpp:1528-1547`）——
        //        两条臂的"泵相对分派的时机"必须是同一个，否则 `fills` 的先后序列不可对齐。
        //   ★ 与 S2 的 `begin()/end()`：`begin()` 在 `run()` 入口、`end()` 在收口（单出口）；
        //     本节只在两者之间 ⇒ 泵永远不会碰已关闭的设备 / 已回填的 WAV（`enabled()` 在 `end()`
        //     之后仍为真，但循环已经退出了 ⇒ 结构上不可能）。
        //   ★ 与 Present 闸门 / 控制层暂停：它们都在**别的**位置、且各自 `continue`/`break`
        //     （Present 闸门在 `present.cpp` 的回调里、控制层闸门在 ⓪− 且 `serve()` 会阻塞）
        //     ⇒ 暂停期间根本走不到本节（⛔ 不泵 ⇒ 不推进设备时钟）；本节也**不写**任何控制层 /
        //     presenter 状态（只写 guest 音频对象 + `stats_`）⇒ 两个方向都不会互相踩。
        //   ★ 记账：`audio_pump_.noteCalled()` 紧跟 `pump()` 之后 ⇒ `calls()` == **真调**的次数
        //     （off 档恒 0 —— 本单最容易悄悄破的一条，读 `LIFT_AOT_AUDIO_TRACE`）。
        //   ★ **off 档的代价** = 最外层那一次 `enabled()` 读 + 一次分支（下面这行 ↓）。⛔ 不要把
        //     `facts` 的计算提到这层 if **外面**：那是每块边界都要付的（300 帧 ≈ 5.4e7 次），
        //     而 `--audio off` 是**缺省档** ⇒ 它的 penalty 必须是"零"级，不是"几纳秒"。
        if (audio_.enabled()) {
            // ★ thread 档的**失败面**：`AudioPumpGate::due()` 在 thread 档恒假 ⇒ 下面那条
            //   `audio_.pump()` 永远不会被调 ⇒ `pump()` 里的 `feeder_error_` 早退在 AOT **不可达**。
            //   ⇒ 这里补一条只读判据（原子读，只在音频开启时付）：feeder 挂了必须**停**并点名，
            //   ⛔ 不许变成"继续跑但没声"。解释器臂仍走 `pump() == Error`，两条臂的失败面同一条。
            if (audio_.threadMode() && audio_.feederFailed()) {
                r.reason = AotStop::Unimplemented;
                r.detail = "音频 feeder 线程失败：" + audio_.feederErrorText();
                break;
            }
            AudioPumpFacts pf;
            pf.enabled = audio_.enabled();
            pf.thread_mode = audio_.threadMode();
            // 与解释器同一个谓词（`machine.cpp:1532` 的三个 negated 条件）；AOT 的嵌套 guest
            // 回调都经 `GuestCallStack`（`cpu_.calls == &calls_`，`aot_host.cpp` 的 prepare）⇒ 一个就够。
            pf.nested_guest_active = calls_.active() || cpu_.shim.timer_firing || seh_.busy();
            pf.insns = cpu_.insns;
            if (audio_pump_.due(pf)) {
                // ★★★ **AOT 臂上 `AudioEngine::pump()` 的唯一调用点** ★★★
                //   它被 `due()` 挡在前面：off 档 / thread 档 / 嵌套 guest 中一律进不来。
                const AudioPump ap = audio_.pump();
                audio_pump_.noteCalled(cpu_.insns);
                if (ap == AudioPump::Suspended) {
                    // ★ 与解释器**同一个形状**，但 AOT 多一步：`pump()` 只改 `cpu_`（`capture` /
                    //   `restore` 都在 `cpu_` 上），而 lifted 代码的真源是 `regs_` ⇒ 必须
                    //   `syncRegsFromCpu()` 把"新 eip / 借走的 esp / 新 fs 现场"回读进 `regs_`，
                    //   再把 `ea` 指到 `sub_4B6C30`。解释器没有这一步是因为它直接跑 `cpu_`。
                    //   ⛔ 少了这一步 = 生成代码从**旧现场**继续 ⇒ 借用的 `[esp]` / 续延点全丢。
                    syncRegsFromCpu();
                    ea = cpu_.eip;
                    continue;
                }
                if (ap == AudioPump::Error) {
                    // ★ 与解释器同一条（`machine.cpp:1542-1545`）：铺帧 / 参数失败 ⇒
                    //   `Unimplemented` 停机并带上 `lastError()`。⛔ 不许静默继续跑（那会把
                    //   "喂料彻底没在跑"变成"游戏卡在没声音"）。
                    r.reason = AotStop::Unimplemented;
                    r.detail = audio_.lastError().empty()
                                   ? std::string("音频泵失败（lastError 为空）")
                                   : ("音频泵：" + audio_.lastError());
                    break;
                }
                // `Idle`：没有待办 / 混音已推进 / feeder 还没注册 ⇒ 下一个边界再来。
            }
        }

        // ---- ⑥ 分派：shim 桩 EA → `lift_shim`；否则派发表 → `lift_cc_<根>`；都不是 ⇒ 响亮停 ----
        //   ★ 桩 EA 的判据与解释器同一份来源：`ShimRegistry` 的桩页（AOT 没有块缓存）。
        //   ★ 形状是**一次无符号减法 + 一次比较**（S1）：
        //       off = ea - stub_page   // 无符号：ea < page ⇒ 巨大值 ⇒ 一定落在页外
        //       off < stub_span        // 1 次 compare
        //     ⛔ 不再单独算 `in_stub_page`（旧代码先花一次 `byStubEa` 的**线性扫描**，
        //     `&&` 顺序让它白付；现在页内才查表，页外连表都不碰）。
        //   ★ 为什么可以**不**测 `(ea & 3) == 0`：登记期断言（`ShimRegistry::checkStubEaShape`，
        //     响亮停）把桩 EA 的形状钉成 `page + slot*4` ⇒ "页内未对齐 ⇒ 一定不是桩 EA"是**前提**。
        //     万一真有一个页内未对齐 EA（只有 guest 跳进桩页中间才可能），槽位索引的二次校验会
        //     拒掉它 ⇒ `def == nullptr` ⇒ `lift_shim` 的 trap 路径（响亮停）。
        //   ★ 槽位索引表在**页内**才被访问：一次数组取 + 一次 `stub_ea` 校验（`byStubEa`，无锁）。
        // ★ 窗口 profile：`t_b` = 分派前（boundary 桶的终点；`lift`/`shim` 桶的起点）。
        if (win_sample) win_b = windowNowNs();
        const std::uint32_t stub_off = ea - stub_page;
        const bool in_stub_page = stub_off < stub_span;
        const ShimDef* def = in_stub_page ? shims_.byStubEa(ea) : nullptr;
        if (in_stub_page) {
            ++r.dispatches;
            ++r.shim_calls;
            profileDispatchSample(ea);
            if (shimTraceOn()) {
                std::fprintf(stderr, "[aot-shim] %llu ins=%-9llu %s!%s stub=%08X ret=%08X\n",
                             static_cast<unsigned long long>(r.shim_calls),
                             static_cast<unsigned long long>(cpu_.insns),
                             def != nullptr ? def->module.c_str() : "(?)",
                             def != nullptr ? def->name.c_str() : "(unknown-stub)", ea,
                             mem_.rd32(cpu_.esp()));
            }
            // ★ `--profile` ④：**shim 归因**（每 `kShimStride` 次**该 shim 自己的**调用采样一次）。
            //   口径与解释器 `--profile` 的 `shimmax` **相反**：这里要**总量**（采样 ns 之和 +
            //   按总调用数折算），不是"最慢一次"。总调用数不在这里记 —— 直接读 `ShimDef::call_count`。
            //   ★ 为什么是"每个 shim 各自"而不是全体调用数：`invoke` 在**入口**才 `++call_count`
            //     ⇒ 这里的 `call_count + 1` 就是本次序号。若按全体 1/1024，调用数只有几百的
            //     `D3D9::Present`（300 次）会拿到 0–1 个样本 ⇒ 它那一行的"总量"整块失真。
            const bool shim_sample =
                prof_.on && def != nullptr &&
                ((def->call_count + 1u) % AotProfile::kShimStride) == 0;
            std::uint64_t shim_t0 = 0;
            if (shim_sample) {
                shim_t0 = profileTicks();
            }
            // ★ S3：**把已解析的 id 递下去**（`def->id + 1`；0 = 不知道 ⇒ 回退 `lift_shim` 的反查）。
            //   `lift_shim` 自己每次都要 `cache->peek`（AOT `cpu.cache == nullptr` ⇒ 恒 miss）+
            //   `idForStub`（旧实现 = 拿锁 + 线性扫 385 项）。这里已经拿到 def ⇒ 不该再付那笔。
            const std::uint32_t shim_id1 = def != nullptr ? def->id + 1u : 0u;
            // ★ 窗口 profile：`shim` 桶**精确**（每次调用 2 次取时；shim 只占派发 ~1.4%）。
            std::uint64_t win_shim_t0 = 0;
            const bool win_shim = win_.on && def != nullptr;
            if (win_shim) win_shim_t0 = windowNowNs();
            const std::uint32_t next = ::lift_shim_id(shim_id1, ea, &regs_, &cpu_);
            if (win_shim) win_.noteShim(def->id, windowNowNs() - win_shim_t0);
            if (win_sample) win_.noteBoundary(win_a, win_b);
            if (shim_sample) {
                const std::uint64_t delta = profileTicks() - shim_t0;
                const std::string key =
                    def != nullptr ? (def->module + "!" + def->name) : std::string("(unknown-stub)");
                auto& st = prof_.shim_by_name[key];
                ++st.n;
                st.ticks += delta;
            }
            // ★ 每帧 `lastHash` 读数（D）：Present 只在 shim 路径上发生；必须在 `!cpu_.running` 与
            //   帧数闸门**之前**打 —— `--max-frames` 恰好是 D3D9 自己在 Present 里置停的那条路，
            //   晚一步就会丢掉最后一帧的读数。
            logPresentedFrames();
            // `lift_shim` 自己把整块状态回读进了 `*st`；`cpu_` 与它一致（同一次调用里同步过）。
            if (!cpu_.running) {
                classifyShimStop(shim_id1, r);
                break;
            }
            if (next == ::kLiftHalt) {
                // `lift_shim` 只有 `running == false` 才返回哨兵 ⇒ 走到这里是内部不一致，仍然响亮停。
                r.reason = AotStop::Unimplemented;
                r.detail = "lift_shim 返回了 kLiftHalt 但 cpu.running 仍为 true（宿主内部不一致）";
                break;
            }
            ea = next;
            // ---- 帧数闸门：present 计数到了 --max-frames ⇒ 停（与 D3D9 内部那条同口径）----
            //   ★ D3D9 的 Present 自己会在 `frameCount() >= max_frames` 时置停（`d3d9.cpp:865-870`），
            //     所以正常情况下上面那条 `!cpu_.running` 先命中；这里兜住 **DDraw Flip 不判**
            //     那条缝（`ddraw_surface.cpp:812` 只 present、不比对）—— 让两条呈现路径都能停。
            if (opts.max_frames != 0 && presenter_.frameCount() >= opts.max_frames) {
                r.reason = AotStop::Stopped;
                r.detail = "max-frames（present 计数达到 --max-frames）";
                break;
            }
            continue;
        }

        std::uint64_t comparisons = 0;
        const LiftFn fn = lookup(ea, &comparisons);
        if (fn == nullptr) {
            // ★ **响亮停**并点名 EA（D10 P4）：⛔ 不设"返回哨兵 = 结束"的约定，也⛔ 不静默当普通 EA。
            r.reason = AotStop::DecodeError;
            if (ea == 0xFFFFFFFFu) {
                r.detail = "EA 0xFFFFFFFF = 栈底返回地址哨兵：guest 一路 `ret` 到了栈哨兵，派发表里没有它"
                           "（⛔ 不当成\"正常结束\"）";
            } else {
                char buf[256];
                std::snprintf(buf, sizeof(buf),
                              "EA %08X 既不在派发表里（closure 缺这一行？）也不是 shim 桩 EA ⇒ 响亮停"
                              "（查找 %llu 次比较；表 %zu 项）",
                              ea, static_cast<unsigned long long>(comparisons), tableSize());
                r.detail = buf;
            }
            break;
        }
        ++r.dispatches;
        ++r.lifted_calls;
        profileDispatchSample(ea);
        if (win_sample) win_.noteBoundary(win_a, win_b);
        const std::uint32_t next = fn(&regs_, mem_.host(0), ea, &cpu_);
        if (win_sample) win_.noteLift(win_b, windowNowNs());
        if (next == ::kLiftHalt) {
            // 生成物里的停机出口：`lift_one` 的 false / `lift_stop`（T1c 的口径）。
            // 解释器把这一类归成 `Unimplemented` + `detail = cpu.stop_why`（T2 §2.2）。
            r.reason = cpu_.running ? AotStop::Unimplemented : hostStopReason(cpu_);
            std::string why = cpu_.stop_why != nullptr ? cpu_.stop_why : "（stop_why 为空）";
            char buf[192];
            std::snprintf(buf, sizeof(buf), "%s（lifted 停机出口 @ %08X）", why.c_str(), ea);
            r.detail = buf;
            break;
        }
        if (!cpu_.running) {
            // 防御：生成物只该通过 `kLiftHalt` 停；真出现"置停但没有哨兵"也要收口（⛔ 不许静默继续）。
            r.reason = hostStopReason(cpu_);
            r.detail = cpu_.stop_why != nullptr ? cpu_.stop_why : "lifted 代码置停但没有返回停机哨兵";
            break;
        }
        ea = next;
    }

    // ---- 收口（只留 guest 语义的读数；真源 machine.cpp:1806-1826）----
    syncCpuFromRegs();
    // ★ 窗口 profile：配方只写了 `profile_begin` 而没写 `profile_end`（或 run 在窗口中途收口）
    //   ⇒ **照样打表 + 关闸门**（⛔ 不静默丢数据；与解释器"未闭合也打印"同口径）。
    if (win_.on) endWindow();
    r.ea = cpu_.eip;
    r.insns = cpu_.insns;
    r.frames = presenter_.frameCount();
    // ★ `--profile`：本轮墙钟长度（只给 `dispatch-top:` 的"占墙钟 x%"用；⛔ 不参与任何判据）。
    if (prof_.on) {
        prof_.wall_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wall_start).count();
    }
    // ---- D28 的新增读数：派发表（开放寻址）的**探测分布**（probes avg / max / cap / load factor）----
    // ★ 为什么落在这里：既有那行 `table : …` 在 `aot_host.cpp`（本轮改动面不含它）⇒ 新读数由
    //   `aot_dispatch.cpp` 自己出、在这里打；`printProbeLine` 只读计数器，⛔ 不影响停机判据。
    printProbeLine(stdout);
    if (r.reason == AotStop::None) {
        // ★ 解释器里这一档是**静默缝**（`RunResult::ok()` 为假 ⇒ 退出码 2 但摘要写 `stop: none`）；
        //   本臂把它变成一条点名的响亮错（⛔ 不给"没原因"留门）。
        r.reason = AotStop::Unimplemented;
        if (r.detail.empty()) r.detail = "执行循环退出但没有停因（宿主内部不一致）";
    }
    // ★ **S2 生命周期 · end**（与解释器 `machine.cpp:1854` 同口径）：关闭 SDL 设备 / 回填 WAV 大小；
    //   此后不再写 guest。★ 与入口的 `begin()` 成对：本函数中"生成物在场"之后的路径只有这一个出口。
    //   ⚠ 取"现在"= 收口这一刻的读数（`present` 行的 tail 之类）不依赖它；⛔ 不要把 `end()` 挪到
    //     `printRunSummary` 之后 —— 摘要里的 `audio     :` 一行读的是**同一份** stats，先 end 再打
    //     才能让 `device=` / `dump=` 反映"这一轮真的开了什么"（统计值不受 end 影响）。
    audio_.end();
    // ---- S3 泵的**收口读数**（opt-in 一行到 stderr；⛔ 缺省档**零输出**）----
    //   ★ 为什么必须留这个口：本单最硬的一条是"**off 档一次都不进 pump**"，而 off 档按定义
    //     **不许有 stdout 输出**（默认档逐字不变）⇒ 那条判据在正式摘要面上**没有**观察点。
    //     这个 env 开关就是它的观察点：`LIFT_AOT_AUDIO_TRACE=1` + **默认 argv** ⇒ `calls=0`
    //     （`calls` = 真调 `AudioEngine::pump()` 的次数，`AudioPumpGate::noteCalled` 唯一记账）。
    //   ★ 为什么是 stderr 而不是 stdout：stdout 的逐字不变是**判据面**，一个 opt-in 读数也不许
    //     混进去（否则"默认档 stdout 与改动前逐字相同"就只能靠"记得别开这个 env"来保证）。
    //   ★ 一次性输出（不是周期性）⇒ 不违反 `AGENTS.md` §12.8 第 3 条；缺省 env 不设 ⇒ 一条都不打。
    if (std::getenv("LIFT_AOT_AUDIO_TRACE") != nullptr) {
        const AudioEngine::Snapshot a = audio_.snapshot();
        std::fprintf(stderr, "[audio-pump] mode=%s calls=%llu fills=%llu fill_bytes=%llu pushed=%llu\n",
                     a.mode, static_cast<unsigned long long>(audio_pump_.calls()),
                     static_cast<unsigned long long>(a.fills),
                     static_cast<unsigned long long>(a.fill_bytes),
                     static_cast<unsigned long long>(a.pushed_bytes));
    }
    // ★ 摘掉 Present 闸门：下一轮 `run()` 可能不带控制层（⛔ 留一条指向旧 Control 的回调）。
    presenter_.setPresentGate(nullptr, nullptr);
    return r;
}

}  // namespace aot
}  // namespace lift
