// emulator-lift · Shape A 的 **AOT 宿主**（bring-up + 执行循环 + 宿主服务）
//
// ★ 它**不是** `Machine` 的包装、也不是它的子集替身 —— 它是"这个宿主恰好要用的那几份底层对象"
//   自己的一份装配：
//     GuestMemory（guest 地址空间）· GuestHeap（guest 堆）· Vfs（guest 世界 → 宿主路径）·
//     ShimRegistry（导入桩）· PeImage（PE 头 / 节表 / 导入表）· GuestLayout / ProcessSetup（地址布局）·
//     X86（cpu 状态）·
//     **T2b 新增**：Decoder · SurfaceArena · HeadlessPresenter · Seh · VirtualClock · HandleTable ·
//     ComRegistry · GuestCallStack · WindowManager · FontEngine · AudioEngine。
//   ⛔ 不 include `machine.h`、⛔ 不实例化 `Machine`、⛔ 不碰 `BlockCache` / `Machine::run`。
//
// ★ 为什么这些类能**脱离 `Machine` 单独实例化**（T2b 逐个核过）：
//   `Machine` 里它们都是**普通成员**（`machine.h:475-543`，缺省构造），构造参数一个都没有；
//   需要外部输入的只有 `SurfaceArena::init(GuestMemory*, base, size)`（`Machine::init` 也是这么调的）
//   与 `AudioEngine::attach(cpu, mem, clock)` / `configure(mode,…)`。⇒ 装配 = 把 `Machine::wire()` /
//   `Machine::init` 里那几行**照原样**再走一遍，⛔ 不伪造、⛔ 不复制语义。
//   ★ 剩下唯一与 `Machine` 绑定的部分是**图形 shim 怎么找到这些对象** —— 那已经由 D4/T2a 解耦成
//     `X86::host`（`runtime/host_services.h`）⇒ 本类实现 `HostServices`，`hostOf(cpu)` 就指向自己。
//
// ★ `cpu_.machine == nullptr`：AOT **有意**没有 Machine。`machineOf()` 因此是 nullptr ——
//   这是**有意**的状态，⛔ 不要去伪造一台空 Machine 来填空。
//   `hasNestedGuestExecutor()` **必须**为 false（AOT 没有认领 `GuestCallStack` 铺的帧的解释器循环；
//   铺了帧就回不来 ⇒ `native_image_upload.cpp` 的调用点会**响亮停机**，那正是想要的形状）。
//
// ★ 执行模型（T2 §1.4 的五件 guest 语义，其余解释器实现细节一律不复刻）：
//   `ea → 派发表 → lift_cc_<根>(regs, mem, ea, cpu)`；shim 桩 EA → `lift_shim`；两者都不是 ⇒ 响亮停。
//   块边界：定时器泵 / SEH 派发 / 两个合成 EA / `--until` / `--max-insns` / `--time-limit` / 帧数闸门。
//
// ★ **native override（D25 第 1 项，2026-11-15 接上）**：函数级 + 段级两张表在**本宿主**上装配，
//   判据与解释器 `machine.cpp:1420-1533` 逐条对齐：
//     · 函数级只在**函数入口 + 真有 call 帧**时拦（`call_target` 的 AOT 等价判据见 `overrideCallFrame`）；
//     · 接管后 `insns += 1 + override 自己的补偿`（`machine.cpp:1456-1459` 的同一笔账）；
//     · fn 返回 false 但置了 `running=false` ⇒ **停机**（⛔ 不是回退）；
//     · 段级只有 `Replaced` 才补 `replaced_insns`（`machine.cpp:1504-1514`）。
//   ★ 开关 = `--native-overrides <list>`（缺省**关**，与解释器缺省同口径 ⇒ 同一个二进制能做 A/B）。
//   ★ `sub_48B090`（`native_image_upload`）**本轮不装**：它要 `startNestedGuestCall()` 重入解释器，
//     而 AOT 的 `hasNestedGuestExecutor()` 恒 false（见上）⇒ 装了就是启动后第一次调用即响亮停机。
//     这一项**只计数**（`override : … skipped-nested=`），⛔ 不为了它去开嵌套执行器。
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "corpus/lst_index.h"
#include "runtime/audio.h"
#include "runtime/clock.h"
#include "runtime/com.h"
#include "runtime/decode.h"
#include "runtime/font.h"
#include "runtime/guest_call.h"
#include "runtime/handles.h"
#include "runtime/heap.h"
#include "runtime/host_services.h"
#include "runtime/host_window.h"
#include "runtime/image.h"
#include "runtime/lift_abi.h"
#include "runtime/memory.h"
#include "runtime/native_overrides.h"
#include "runtime/present.h"
#include "runtime/seh.h"
#include "runtime/shims.h"
#include "runtime/surface.h"
#include "runtime/vfs.h"
#include "runtime/x86.h"

namespace lift {
namespace aot {

// =====================================================================================
// AOT 的**自有**停机 / 选项类型（⛔ 不 include `machine.h` ⇒ 不能复用 `RunOptions` / `RunResult`）
//
// ★ 值域与名字**照 `machine.h:46-82` 的 `StopReason` 抄一份**（D3 想把枚举搬去
//   `runtime/stop_reason.h` 让两臂共用；本轮那份还没落地 ⇒ 这里是第二份）。⛔ 不许在这里
//   发明新的停因：每一条都对应解释器的一条**已有**判据（见 T2 §2.2 的全表）。
//   ⚠ 两份枚举会漂 ⇒ 这是**留给父级的裁决**（D3 落地后删掉这份）。
enum class AotStop : std::uint8_t {
    None = 0,              // 循环退出但没给原因（解释器里这是静默缝 ⇒ 本臂把它变成响亮错）
    Stopped,               // 实现里主动停（ExitProcess / --max-frames / 影片放完 …）⇒ **正常**
    MaxInsns,              // --max-insns（正常）
    UntilEa,               // --until ea（正常）
    TimeLimit,             // --time-limit（正常）
    Unimplemented,         // 未实现的导入 / 指令执行失败
    DecodeError,           // 取指失败（本臂 = EA 不在派发表里 / 不在 shim 桩页）
    NoCode,                // shim 块指向不存在的 id / 空块
    ResourceExhausted,     // 我方 arena / guest 堆不够
    Trap,                  // 已注册的槽 / 形态没实现（含"缺执行能力"）
    UnhandledException,    // SEH 走不完
    NonContinuableException,  // EXCEPTION_NONCONTINUABLE 被要求继续执行
};

const char* aotStopName(AotStop r);
// `ok()` 的口径与 `machine.h:147-149` 相同：UntilEa / MaxInsns / Stopped / TimeLimit（本臂没有
// FirstShim）。
bool aotStopOk(AotStop r);

// ★ `--skip-logo` 的两个 guest 事实（锚 **EA**，不锚行号；只对 D1 pin 住的那一份 exe 成立）：
//   · 钩子点 = `0x004BAA33` —— `call sub_415640` 的 **resume EA**（生成器把每条 call 的下一条 EA
//     加进入口集 ⇒ 它在派发表里）。⚠ **不能**照抄解释器的 `0x004BAA37`：那只是同一函数内部的块
//     （`bb_4BAA37` 由 `bb_4BAA33` 的 `jmp short` 直接并入），派发表里**没有**它 ⇒ 挂那里永不命中。
//     两点之间只有一条 `jmp short`（无语义）⇒ 在这里写那一格与解释器在 `0x4BAA37` 写等价，
//     且同样发生在 `0x4BAA3A` 的 `mov ds:dword_55E1BC, eax`（指针发布）**之前**。
//   · 字段 = `T_Command + 0x5EB5C`（解释器 `machine.cpp:38-39` 的 `kShowLogoInitEa` /
//     `kShowLogoFieldOffset` 的**同一个字段**，只换钩子 EA）。
constexpr std::uint32_t kSkipLogoHookEa = 0x004BAA33u;
constexpr std::uint32_t kSkipLogoFieldOffset = 0x0005EB5Cu;

struct AotRunOptions {
    std::uint32_t until_ea = 0;
    std::uint64_t max_insns = 0;     // 绝对值（与解释器同口径：`cpu.insns >= max`）
    std::uint32_t max_frames = 0;    // 0 = 不限；Present 计数达到即停
    double time_limit_sec = 0.0;     // 0 = 不限（每 4096 条查一次墙钟，与解释器同粒度）
    // ★ `--skip-logo`：一次性 latch，语义与解释器 `Machine::setSkipLogo` **逐条相同**
    //   （`machine.cpp:1377-1401`；对同一份 exe pin 住的那一格 `T_Command + 0x5EB5C` 写 0）。
    //   ⛔ 不跳过 boot / 不改 eip / 不改任何寄存器。AOT 的钩子点是 `0x004BAA33`（见 aot_run.cpp）。
    bool skip_logo = false;
};

struct AotRunResult {
    AotStop reason = AotStop::None;
    std::string detail;
    std::uint32_t ea = 0;         // 收口时的 eip
    std::uint64_t insns = 0;      // `cpu.insns`（跨 run 累计，与解释器同一份）
    std::uint64_t frames = 0;     // `presenter().frameCount()`（跨 run 累计）
    std::uint64_t dispatches = 0; // 本轮的 lift / shim 分派次数
    std::uint64_t lifted_calls = 0;
    std::uint64_t shim_calls = 0;
    // ---- native override 的账（D25 第 1 项；字段口径 = 解释器 `machine.cpp:1436-1513`）----
    // ★ `ov_insns` = override **折算成多少条 guest 指令**（= 每次接管 `++insns` 一条 + override
    //   自己补的条数 + 段级的 `replaced_insns`）。⛔ 漏掉它，本臂的 `insns` 与解释器臂就不可比。
    std::uint64_t ov_insns = 0;
    // 到达"需要嵌套 guest 执行器"的 override 入口（且有 call 帧）但本宿主没有嵌套执行器 ⇒ 跳过。
    // ★ 0 也要打（"没跳过"与"根本没走到"是两件事）。
    std::uint64_t ov_skipped_nested = 0;
    // 到达 override 入口但**不是 call 帧** ⇒ 不许弹返回地址，走 lifted 代码。
    // （解释器同一条判据：`call_target != 0 && cpu_.eip == call_target`，`machine.cpp:1431`）
    std::uint64_t ov_not_call = 0;
    bool ok() const { return aotStopOk(reason); }
};

// =====================================================================================
class AotHost final : public HostServices {
public:
    AotHost() = default;
    // 内含 4 GiB 虚拟预留（`GuestMemory`）与 shim 注册表（互斥量）/ 堆块表 ⇒ 不支持拷贝。
    AotHost(const AotHost&) = delete;
    AotHost& operator=(const AotHost&) = delete;

    // ① 平台 + guest 地址空间：4 GiB 预留（构造时已做，这里失败要能报出来）+ 平台 fault handler
    //    （由 `GuestMemory::reserve` 装）+ `LIFT_SAFE_MEMORY` 同口径 + surface arena + 把**全部**
    //    服务指针接到 cpu 上（`Machine::init` 的 surfaces + `Machine::wire` 的接线）。
    bool init(std::string* err);

    // ② 载入 PE：解析头 / 节表 / 导入表 / entry，把节按 `image_base + RVA` 映射进 guest。
    //    ★ D1：**先核对**构建期 pin（sha256 + entry）再映射 —— 不一致 ⇒ 返回 false 且
    //      `pinMismatch() == true`（调用方 exit 2）。
    bool loadImage(const std::string& exe_path, std::string* err);

    // ③ 装配 guest 进程结构（栈 / TEB / PEB / TLS / 命令行 / 环境块）+ guest 堆 + shim 桩页
    //    注册与 bind + cpu 初始寄存器。**不 installShim**（AOT 没有 BlockCache：桩 EA → id 由
    //    `ShimRegistry::byStubEa` / `lift_shim` 自己反查）。
    bool prepare(std::string* err);

    // ③' AGERC.DLL：作为**真实 guest 模块**装载（真源 `Machine::loadAgercModule`，
    //    `machine.cpp:404-493`）。⛔ 不是"可选彩蛋"：guest 启动早期就 `LoadLibraryA("AGERC.DLL")`，
    //    没有镜像 ⇒ 它返回 0 + 126 ⇒ guest 走**错误对话框**路径（实测：AOT 会停在
    //    `poly:005C3627 = j_MessageBoxA_15` 这个 thunk 上，而解释器从这里继续 `GetProcAddress`）。
    //    必须在 `prepare()` 之后调（要 heap / stack / layout / 主镜像都已就位才能算冲突区）。
    bool loadAgercModule(const std::string& path, std::string* err);

    // ④ 执行循环（T2 §1.4 的五件 guest 语义；见 cpp 的函数头注）。
    AotRunResult run(const AotRunOptions& opts);

    // ③'' `--lst`：**只为 native override 的 EA 解析 / 段级锚校验**（口径同解释器 `main.cpp:882-896`：
    //    执行源与语料不是同一份 ⇒ 拒绝装配）。⛔ 执行不需要它：`cpu_.lst` 只作诊断文本框
    //    （与解释器同口径 —— fault 报告能看见指令文本）。
    bool loadLst(const std::string& path, std::string* err);
    bool lstLoaded() const { return lst_loaded_; }
    const LstIndex& lst() const { return lst_; }

    // ③''' native override 两张表的装配（真源 `Machine::configureNativeOverrides`，`machine.cpp:3190-3309`）。
    //    `list` 的语法与解释器**逐字相同**：逗号分隔的名字 / `all` / `-name` 排除。
    //    ★ 与解释器的**唯一**差别（登记在这里，⛔ 不藏）：需要嵌套 guest 执行器的项（`sub_48B090`）
    //      本轮**不装**，只按到达次数记 `AotRunResult::ov_skipped_nested`；理由见文件头注。
    bool configureNativeOverrides(const std::string& list, std::string* err);
    const NativeOverrideTable& overrides() const { return overrides_; }
    const NativeBlockTable& blockOverrides() const { return block_overrides_; }
    const std::string& overridesSpec() const { return overrides_spec_; }
    // 因缺嵌套执行器而**没装**的那些项（名字；读数用）。
    const std::vector<std::string>& nestedOnlyNames() const { return nested_only_name_; }

    // ④' 显示 / GPU 后端：**在第一条 guest 指令之前**接到宿主服务上（真源 `Machine::setGpuRaster`）。
    //    ★ 语义与 `Machine::gpuRaster()` 逐字相同（`machine.cpp:3311-3315`）：显式设置的指针优先，
    //      否则取 presenter 的 sink。`nullptr` = 没有 GPU 光栅后端（3D 一个像素都不画 —— 那是
    //      `d3d9_pipeline.cpp:3983` 的拒批，不是"画得不一样"）。
    //    ★ `aot_main.cpp` 两个档位（headless / window）都设：解释器的 `--present headless` 也建
    //      **隐藏窗口 + GPU device**（`main.cpp:724-726`）⇒ 两臂的像素路径才会是同一个。
    void setGpuRaster(GpuRaster* raster) { gpu_raster_ = raster; }

    // ⑤ 读数（bring-up 结束时的"起点"快照）。
    void printReadout(std::FILE* out) const;
    // ⑥ 跑完的摘要（停因 / EA / insns / frames / 派发表读数）。
    void printRunSummary(const AotRunResult& r, std::FILE* out) const;

    // ---- 读数取用 ----
    const PeImage& image() const { return image_; }
    const GuestLayout& layout() const { return layout_; }
    const ProcessSetup& setup() const { return setup_; }
    const X86& cpu() const { return cpu_; }
    X86& cpu() { return cpu_; }
    const ShimRegistry& shims() const { return shims_; }
    const GuestMemory& mem() const { return mem_; }
    const GuestHeap& heap() const { return heap_; }
    // ★ `surfaces()` / `presenter()` / `seh()` 只在 `HostServices` 那一节实现一次（`override`）——
    //   ⛔ 这里不再写第二份 const 访问器：同签名重复声明是编译错，而"两份取用面"本身也是漂的来源。
    // ★ VFS 的**可写**入口：`--game-dir` / `--appdata` / `--guest-root` 只能在 `init()` 之后、
    //   `prepare()` 之前配（`prepare` 里的 `setupProcessStructures` 要用 `guestExePath()` /
    //   `guestEnvironment()` 造 guest 的命令行 / 环境块）。⛔ 不在 `init()` 里假设 CLI 形态。
    Vfs& vfs() { return vfs_; }
    const Vfs& vfs() const { return vfs_; }
    const std::string& exePath() const { return exe_path_; }
    // AGERC（真实 guest 模块）的装载状态 / 基址 / 镜像（读数与 shim 面）。
    bool agercLoaded() const { return agerc_loaded_; }
    std::uint32_t agercBase() const { return agerc_base_; }
    const PeImage& agercImage() const { return agerc_image_; }

    bool initialized() const { return initialized_; }
    bool imageLoaded() const { return image_loaded_; }
    bool prepared() const { return prepared_; }
    // D1 的核对结果：true = 载入的 PE 与构建期 pin **不一致**（调用方 exit 2）。
    bool pinMismatch() const { return pin_mismatch_; }
    bool pinChecked() const { return pin_checked_; }

    // ---- `HostServices`（shim 侧唯一的取用面；见 runtime/host_services.h）----
    SurfaceArena& surfaces() override { return surfaces_; }
    const SurfaceArena& surfaces() const override { return surfaces_; }
    HeadlessPresenter& presenter() override { return presenter_; }
    const HeadlessPresenter& presenter() const override { return presenter_; }
    Seh& seh() override { return seh_; }
    const Seh& seh() const override { return seh_; }
    Decoder& decoder() override { return decoder_; }
    // ★ 与 `Machine::gpuRaster()` 同一判据（`machine.cpp:3311-3321`）：显式设置的后端优先，
    //   否则取 presenter 的 sink。⛔ 不再硬 `nullptr` —— 那会让 3D 一个像素都不画（W0 C10 Ⓐ）。
    GpuRaster* gpuRaster() override;
    const GpuRaster* gpuRaster() const override;
    // `--close-confirm` 是 AGERC `_ShowDialog@12` 的关窗策略；AOT 本轮没有这个开关 ⇒ 取缺省 Ask
    // （与 `Machine` 未配置时同值）。
    CloseConfirmPolicy closeConfirmPolicy() const override { return CloseConfirmPolicy::Ask; }
    // ★ AOT **没有**嵌套 guest 执行器（没有解释器 run 循环会认领 `GuestCallStack` 铺的帧）⇒ false。
    //   消费点 `native_image_upload.cpp` 会因此**响亮停机**（⛔ 不静默落回解释器）。
    bool hasNestedGuestExecutor() const override { return false; }
    // 归属判据 = "这台 cpu 就是本宿主装配的那一份"（与 `Machine::ownsCpu` 同形）。
    bool ownsCpu(const X86& cpu) const override { return &cpu == &cpu_; }

private:
    // 块边界的宿主泵（三个合成 EA：`0x7FFEF008` 续延 / `0x7FFEF004` 定时器返回 / SEH 派发）。
    // ★ 这三个 EA 都**落在 shim 桩页范围内**（0x7FFE0000 + 0x2000 槽）⇒ 必须在"桩 EA ⇒ lift_shim"
    //   之前认出来，否则会被当成普通桩 EA 而落到 `lift_shim` 的"桩 EA 不在注册表"响亮停上
    //   —— 那是**归因错**（缺的是合成 EA 的执行能力，不是 guest 的错）。
    // `Handled` ⇒ 调用方按 `cpu_.eip` 继续；`Stop` ⇒ `r.reason` 已写、本轮收口。
    enum class Pump : std::uint8_t { NotHandled, Handled, Stop };
    Pump pumpSyntheticEas(AotRunResult& r);
    // 到期的 winmm 定时器在块边界铺回调帧（拆自 `machine.cpp:2140-2199` 的 `Machine::fireDueTimer`）。
    // 返回 true = 铺了帧（调用方继续）；返回 false 且 `r.reason != None` = 收口。
    bool fireDueTimer(AotRunResult& r);
    // SEH 派发（拆自 `machine.cpp:2208-2280` 的 `Machine::pumpSeh`；影子栈部分不要 —— 那是 profiler）。
    Pump pumpSeh(AotRunResult& r);
    // shim 停机归类（真源 `machine.cpp:1651-1675`；⛔ 三条判据的顺序不许换）。
    void classifyShimStop(const ShimDef* def, AotRunResult& r);
    // 宿主置停的三分类（真源 `machine.cpp:75-80` 的 `hostStopReason`；那份是内部链接，抄判据不抄代码）。
    static AotStop hostStopReason(const X86& cpu);
    // `regs` ↔ `cpu` 的同步（lifted 侧的状态真源是 `regs`；宿主侧逻辑读 `cpu`）。
    void syncCpuFromRegs();
    void syncRegsFromCpu();

    // ---- native override 的派发前置判据（跑在**每一次派发**上，所以必须便宜）----
    // ★ `overrideCandidate(ea)` = 位图**前置过滤**：位=1 才做线性查找（≤20 项），位=0 一定没有。
    //   `ea >> 2` 直接索引 512 Kbit = 64 KiB（`.text` 1.79 MB ⇒ 448K 个槽，全部落在表内）。
    //   ⛔ 位图不是"命中表"：同一位可能被别的 EA 共享 ⇒ 只会多查一次，**不会漏**
    //   （这是它敢当过滤器的唯一理由）。
    void rebuildOverrideFilter();
    bool overrideCandidate(std::uint32_t ea) const;
    // ★ "这真的是一个 call 帧吗"的判据 —— 解释器用 `call_target != 0 && cpu_.eip == call_target`
    //   （`machine.cpp:1431`）。AOT 的 trampoline 只知道"下一个 EA"，**不知道**上一个块是怎么离开的
    //   （`emitCall` 与 `emitEdge` 都发 `ret i32 <EA>`，见 `lift_gen_emit.cpp:540-559`），所以按
    //   **guest 栈上的返回地址**反查：`[esp]` 必须是某个 `call rel32`（`E8 rel32`，5 字节）的
    //   下一条指令地址，且那个 call 的目标正好是本 EA。
    //   ★ 为什么这条等价于解释器的判据：只有真 call 才会把"本站点的下一条 EA"压进 `[esp]`；
    //     尾跳 `jmp` 的 `[esp]` 是**调用者的**返回地址（那个 call 的目标不是本 EA）⇒ 一定不匹配。
    //   ★ 代价：一次命中 EA 才做（`overrideCandidate` 之后），5 字节读 + 4 字节比较。
    //   ★ 已知不覆盖（登记，⛔ 不猜着补）：`call reg/mem`（`FF /2`）到达的 override 入口 ——
    //     那种 call 的返回地址不在 `[esp]-5` 上 ⇒ 走 lifted 代码（**性能**缺口，不是语义缺口）。
    bool overrideCallFrame(std::uint32_t callee) const;

    // ★ 顺序 = 依赖顺序（`cpu_` 里的服务指针指向下面这些对象 ⇒ 它们必须先存在）。
    GuestMemory mem_;
    GuestHeap heap_;
    Vfs vfs_;
    ShimRegistry shims_;
    Decoder decoder_;
    PeImage image_;
    PeImage agerc_image_;  // AGERC.DLL 的真实镜像（`cpu_.shim.agerc_image` 指向它）
    GuestLayout layout_;
    ProcessSetup setup_;
    HandleTable handles_;
    VirtualClock clock_;
    ComRegistry com_;
    GuestCallStack calls_;
    WindowManager windows_;
    FontEngine fonts_;
    AudioEngine audio_;
    SurfaceArena surfaces_;
    HeadlessPresenter presenter_;
    Seh seh_;
    X86 cpu_;
    // lifted 代码的寄存器现场（唯一真源；`lift_shim` / `lift_one` 与它双向同步）。
    LiftRegs regs_{};

    std::string exe_path_;  // 宿主侧的 `--exe`（只进 trace 输入清单，⛔ 不进 guest 内存）
    std::string agerc_path_;
    std::uint32_t agerc_base_ = 0;
    bool agerc_loaded_ = false;
    bool initialized_ = false;
    bool image_loaded_ = false;
    bool prepared_ = false;
    bool pin_checked_ = false;   // 这一份 PE 是否已经过 pin 核对
    bool pin_mismatch_ = false;  // 核对没通过（D1：exit 2）
    // 恢复判定的"只应用一次"（真源 `machine.cpp:1369`；常量 `kRecoveryCheckEa` 在 cpp 里）
    bool recovery_bypass_checked_ = false;
    bool recovery_bypass_applied_ = false;
    std::uint32_t recovery_bypass_prior_eax_ = 0;
    // ★ `--skip-logo`（`AotRunOptions::skip_logo` 在 `run()` 开头 arm）：一次性 latch。
    //   四个字段与 `machine.h:555-558` 的 `skip_logo_*` **一一对应**，判据与 `machine.cpp:1377-1401`
    //   逐条相同（唯一差别是钩子 EA = `0x004BAA33`，见 aot_run.cpp 的注释）。
    bool skip_logo_ = false;
    bool skip_logo_checked_ = false;
    bool skip_logo_applied_ = false;
    std::uint32_t skip_logo_object_ = 0;
    std::uint32_t skip_logo_prior_value_ = 0;  // 写前的 +0x5EB5C（读数用；解释器只 warn 不打值）
    // ★ GPU 光栅化后端（`Machine::gpu_raster_` 的同形）：由 `aot_main.cpp` 在第一条 guest 指令之前设。
    //   所有权**不在这里**（`SdlWindow` 由 main 的 `display` / presenter 的 sink 持有）。
    GpuRaster* gpu_raster_ = nullptr;
    // SEH 展开**不许跨过宿主 guest 回调帧**的判据（真源 `machine.cpp:2208-2236`）：
    // 0 = 不在任何回调里；非 0 = 最内层回调帧的返回槽 EA。影子栈那块是 profiler，⛔ 不复刻。
    std::uint32_t seh_guest_boundary_ = 0;
    // 三个合成 EA 的命中数（读数用：0 = 本轮根本没撞到 —— ⛔ 不拿"实现了"冒充"跑过了"）。
    std::uint64_t timerFires_ = 0;   // 定时器回调铺帧次数
    std::uint64_t callResumes_ = 0;  // guest 回调续延次数

    // ---- native override（D25 第 1 项）----
    // ★ 三份真源：`overrides_` / `block_overrides_`（与解释器**同一份类型、同一份实现**），
    //   `lst_` 只用来"名字 → EA"与段级锚校验（⛔ 不硬编码 EA）。
    LstIndex lst_;
    bool lst_loaded_ = false;
    NativeOverrideTable overrides_;
    NativeBlockTable block_overrides_;
    std::string overrides_spec_;
    // 需要嵌套 guest 执行器 ⇒ 本轮**不装**、只计数的项（EA 同样由 `.lst` 解析）。
    std::vector<std::uint32_t> nested_only_ea_;
    std::vector<std::string> nested_only_name_;
    // 派发前置过滤位图（见 `overrideCandidate`）。空 = 没有 override ⇒ 每次派发只判一次 `empty()`。
    static constexpr std::size_t kOverrideFilterBits = std::size_t{1} << 19;  // 512 Kbit = 64 KiB
    std::vector<std::uint64_t> override_filter_;
    // "到达 override 入口但不是 call 帧"的 EA 名单（每个 EA 只打一行诊断用）。
    // ★ 为什么值得记：`call reg/mem`（虚表调用）的返回地址不在 `[esp]-5` 上 ⇒ 那些到达会被判据
    //   拒掉。**这是性能缺口不是语义缺口**，但"到底有多少、落在哪些 EA"必须看得见。
    std::vector<std::uint32_t> not_call_logged_;
};

}  // namespace aot
}  // namespace lift
