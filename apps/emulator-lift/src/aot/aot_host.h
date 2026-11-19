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
// ★ **执行来源只有两个**：构建期 lift 出来的 IR，与 shim。⛔ 没有"按 EA 把 guest 函数体交给宿主 C++
//   重实现"的第三条路（那套机制已于 2026-11-16 删除：它替换的是**执行来源**而不是 IR，
//   留着就等于给"跑通"开口子 —— 跑通的可能是手写 C++，不是被 pin 住的那份 lifted 代码）。
#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "aot/aot_hook_ea.h"
#include "aot/frame_bucket.h"  // 按帧 shim 分桶（`--profile-slow-frames`；纯逻辑 + 有界状态机）
#include "aot/window_prof.h"  // 窗口 profile 的桶与闭式（纯头；可被 pure CTest 直接驱动）
#include "runtime/audio.h"
#include "runtime/audio_pump_gate.h"  // S3 泵闸门（纯头；可被 pure CTest 直接驱动）
#include "runtime/clock.h"
#include "runtime/com.h"
#include "runtime/control_snapshot.h"  // 控制层快照的值类型 + `ControlHostSide`（⛔ 无 beast）
#include "runtime/critical_sections.h"  // guest CRITICAL_SECTION ↔ 宿主递归锁（feeder 的硬前提）
#include "runtime/decode.h"
#include "runtime/engine_frames.h"  // 帧列表读数源（`game.local_frames()`；纯读 guest）
#include "runtime/engine_marker.h"  // 控制层 `sample` 的脚本身份采样器
#include "runtime/font.h"
#include "runtime/guest_call.h"
#include "runtime/handles.h"
#include "runtime/heap.h"
#include "runtime/host_services.h"
#include "runtime/host_window.h"
#include "runtime/image.h"
#include "runtime/lift_abi.h"
#include "runtime/memory.h"
#include "runtime/present.h"
#include "runtime/seh.h"
#include "runtime/shims.h"
#include "runtime/surface.h"
#include "runtime/vfs.h"
#include "runtime/x86.h"
// ★ 宿主能力表 `ui::UiHostOps`（切片 3）：`frame_source_` 那一路上已经经 `engine_frames.h` 拿到
//   `ui/script.h`，这里**显式**写一行 —— 成员类型是它，别把依赖留给传递 include。
#include "ui/script.h"

namespace lift {
// ★ 控制层端点（`runtime/control.h`）：这里只**前置声明** —— ⛔ 不 include control.h 的实现面，
//   更不 include beast / nlohmann（那是 `runtime/control_ws.cpp` 一个 TU 的事）。
class Control;
}  // namespace lift

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
//   · 钩子点 = `kSkipLogoHookEa`（`aot/aot_hook_ea.h` —— **唯一一份**常量；S7 之后生成器也读它，
//     以保证这个 EA 留在派发边界上，见那份头注）。
//   · 字段 = `T_Command + 0x5EB5C`（解释器 `machine.cpp:38-39` 的 `kShowLogoInitEa` /
//     `kShowLogoFieldOffset` 的**同一个字段**，只换钩子 EA）。
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
    // ★ `--profile`：**缺省关**。关着时执行循环里连一个计数器都不加（见 `AotProfile` 的三条硬口径）。
    //   开着的全部产物都落在 stdout 末尾的 `=== profile (sampling: …) ===` 块 + 每帧一行 `frame-ms:`。
    bool profile = false;
    // ★ `--profile-slow-frames <ms>`：**缺省 0 = 关**；CLI 要求**必须与 `--profile` 同时给**
    //   （否则 exit 2 —— 它没有自己的时间轴，离开 `--profile` 连帧时间戳都没有）。
    //   >0 ⇒ 每帧结束时若该帧宿主墙钟 ≥ 它，就把**这一帧**的 shim 时间按名字分桶存下来，
    //   run 结束时打**一张**表（⛔ 不每帧打）。口径 / 有界 / 主线程口径见 `aot/frame_bucket.h`。
    //   ★ 0 = 关 ⇒ 执行循环里一次取时都不多做（与"没给这个 flag"逐条相同）。
    std::uint64_t profile_slow_frames_ms = 0;
    // ★ `--frame-log`：**缺省关**。开档才打逐帧 `[frame] n=<帧号> hash=<guest 后缓冲哈希>`。
    //   为什么缺省关：行数 = 帧数（300 帧 ⇒ 300 行，占默认 stdout 的 78%），而它只是**画面序列**读数
    //   （"在动 / 卡住同一帧"），不是判据；末帧 hash 另有一份（摘要的 `present : … lastHash=`）。
    //   ★ 闸门 = `frame_log || prof_.on`：`--profile` 档**逐字不变**（那是已有存档的可比面）。
    bool frame_log = false;
    // ★ `--region-log`：**缺省关**。开档才打 `printReadout` 末尾的 guest 区域表
    //   （`regions   :` 一行 + 每段一行，共 **28 行**；`dumpRegions` 复用 `runtime/memory.cpp` 那份）。
    //   为什么缺省关（用户 2026-11-16 口径："region 信息是多余的噪音，考虑 opt-in 而非默认"）：
    //   那 28 行是**静态布局的快照**（谁占了哪一段），一次 run 里不变，而同一个问题用
    //   `heap` / `stack` / `va` 那几行 + 复跑一次 `--region-log` 就能回答 ⇒ 属噪音。
    //   ★ 形态与 `--frame-log` **同族**：一类读数的 opt-in、⛔ 不是"总开关"（别的 readout 一行不动）。
    bool region_log = false;
    // ★ **外部请求停止**（缺省 nullptr = 从不请求）。唯一的生产者 = `--input-plan` 的 `quit`
    //   （`InputPlan::quitFlag()`）⇒ 与解释器臂**同一条口径**（`machine.cpp` 的
    //   `interaction_.actuator().quitRequested()` ⇒ `Stopped` + detail `… quit`）：
    //   本次运行以 `Stopped`（正常结局）收口，⛔ **不等 guest 自己退出** —— 那会把 guest
    //   带进它的停机路径，而停机路径可能不在闭包里（实测：WM_QUIT 之后停在 `00513B90`
    //   ⇒ `decode-error`，退出码 2）。
    //   读点在**主线程**里写这个 bool、主线程的块边界读它 ⇒ 不需要原子。
    const bool* stop_requested = nullptr;
};

struct AotRunResult {
    AotStop reason = AotStop::None;
    std::string detail;
    // ★ 控制层 `bye.reason` 的**宿主停机原因**（空 = 按 `reason` 映射）。唯一生产者 = 窗口 sink
    //   的"暂停期间的关窗 / ESC"（`PresentSink::controlStopReason()`），值 `"window-closed"`。
    std::string host_stop_reason;
    std::uint32_t ea = 0;         // 收口时的 eip
    std::uint64_t insns = 0;      // `cpu.insns`（跨 run 累计，与解释器同一份）
    std::uint64_t frames = 0;     // `presenter().frameCount()`（跨 run 累计）
    std::uint64_t dispatches = 0; // 本轮的 lift / shim 分派次数
    std::uint64_t lifted_calls = 0;
    std::uint64_t shim_calls = 0;
    bool ok() const { return aotStopOk(reason); }
};

// =====================================================================================
// 采样式内部 profile（`--profile`；**缺省关 ⇒ 执行循环里连一个计数器都不加**）
//
// ★ 三条硬口径（父级任务书）：
//   ① **默认关闭**：没有 `--profile` 时行为 / 产物 / 输出**逐字不变**（新增的只有 `--help` 那一行）。
//   ② **采样，不逐次**：三个固定 stride（下面常量），⛔ 不在热路径上对每一次调用计时。
//      为什么必须采样：`lift_one` 单次 70–150 ns，而两次 `steady_clock::now()` 本身就是 ~40 ns
//      ⇒ 逐次计时是与被测对象同量级的仪器。1/1024 把它压到 ~0.1%。
//   ③ **不改任何语义**：只读计数器；`frames` / `stop` / `misses` / arena / 末帧 hash / `insns`
//      在开/关两档必须逐位一致（父级验证 3 就是拿这两档对账）。
//
// ★ 读数的四个落点（都在 stdout 里、都有固定前缀，`Select-String` 能抓）：
//   `frame-ms:`（每帧一行，Present 时打）/ `dispatch-top:`（top-20）/
//   `liftone:`（op 汇总 + top-20 (op,EA)）/ `shim:`（top-15）/ `freq:`（机器频率校准）。
// =====================================================================================
struct AotProfile {
    // ---- 采样率（固定间隔；**会打进输出头**，⛔ 不把数字抄进散文）----
    static constexpr std::uint64_t kDispatchStride = 4096;  // 每 N 次派发采样一次（自计时间隔法）
    // ★ `lift_one` 的 stride **只有一份**：`lift::kLiftOneProfileStride`（`runtime/lift_abi.h`）——
    //   计数器在 `lift_abi.cpp` 里（`lift_one` 是生成物直接 call 的，宿主看不见），这里只引用它。
    static constexpr std::uint64_t kLiftOneStride = lift::kLiftOneProfileStride;
    // ★ shim 的 stride 是**每个 shim 各自**的调用序号（用 `ShimDef::call_count`），不是全体调用数。
    //   为什么不用 1/1024：全体 1/1024 下，调用数只有几百的 shim（`D3D9::Present` = 300 次）拿到
    //   0–1 个样本 ⇒ "总量"会整块失真（实测 on1 恰好 1 个样本、3.8 ms/次 ⇒ 折算 1.15 s，
    //   而 on2 里它连 top-15 都没进）。按每个 shim 自己的序号采样后，Present 也能拿到 ~5 个样本。
    //   代价：总样本 ≈ Σ调用/64 ≈ 3.5k 次取时（< 0.2 ms）。
    static constexpr std::uint64_t kShimStride = 64;

    bool on = false;
    // ---- 机器频率校准（cycle → ms）----
    // ★ `tsc_ok` = 这个平台能读 TSC。TSC 是**恒定速率**（一般 = 标称基频）⇒ 即使核心降频，
    //   `ticks / ghz` 仍是墙钟时间；这就是为什么必须实测而不是硬编码 3.2 GHz。
    bool tsc_ok = false;
    double ghz = 0.0;           // 实测 TSC 频率（GHz）；0 = 不可用
    double calib_ms = 0.0;      // 校准忙循环的墙钟长度（ms）
    std::uint64_t calib_ticks = 0;  // 校准忙循环里走过的 ticks
    // 仪器自身的一次取时开销（ns；实测，不猜）—— 读数时必须知道它是不是与 `lift_one` 同量级。
    double clock_read_ns = 0.0;
    // 本轮 `run()` 的墙钟长度（ms；`run()` 收口时写）—— 只给"占墙钟 x%"用。
    double wall_ms = 0.0;
    // ---- ① 每帧时间戳 ----
    std::uint64_t frame_lines = 0;
    // ---- ② 派发归因（自计时间隔法：区间消耗记到**该次采样的入口 EA** 上）----
    std::uint64_t dispatch_calls = 0;    // 本轮派发总次数（含 shim；只作读数，⛔ 不参与语义）
    std::uint64_t dispatch_seen = 0;     // 距上次采样的派发计数（到 stride 就清零）
    std::uint64_t dispatch_samples = 0;  // 真正取到的样本数（首个样本只立基线）
    std::uint64_t dispatch_last = 0;     // 上一次采样的 ticks
    std::uint64_t dispatch_total = 0;    // 采样区间累计（ticks；`tsc_ok == false` 时是 ns）
    // ea → {样本数, ticks}
    std::map<std::uint32_t, std::pair<std::uint64_t, std::uint64_t>> dispatch_by_ea;
    // ---- ③ lift_one 归因（按 EA 聚合；一个 EA 只对应一条静态指令 ⇒ op 由样本带入）----
    struct LiftOneStat {
        std::uint64_t n = 0;    // 样本数
        std::uint64_t ns = 0;   // 采样到的单次耗时之和
        std::string op;         // 该 EA 的 op 名（`opName(in.op)`；首次样本记下）
    };
    std::uint64_t liftone_samples = 0;
    std::map<std::uint32_t, LiftOneStat> liftone_by_ea;
    // ★ 四桶归因（A 换入 / B 解码 / C 执行 / D 回读）：**按 op** 累计（样本数与 `liftone_samples` 同一个集合）。
    //   ⛔ 只用于归因（"慢在哪一段"），不进任何判据；每段含一次取时（~20 ns 地板，见 `clock_read_ns`）。
    static constexpr int kLiftOnePhaseCount = 4;
    std::map<std::string, std::array<std::uint64_t, kLiftOnePhaseCount>> liftone_phase_by_op;
    std::array<std::uint64_t, kLiftOnePhaseCount> liftone_phase_total{};
    // ---- ④ shim 归因（**总量**，不是最慢一次 ⇒ 与解释器 `shimmax` 不同）----
    struct ShimStat {
        std::uint64_t n = 0;      // 样本数
        std::uint64_t ticks = 0;  // 采样到的耗时之和（TSC ticks；非 TSC 平台是 ns）
    };
    std::uint64_t shim_seen = 0;  // 预留（当前实现按每个 shim 自己的 `call_count` 采样，不用这个）
    // "module!name" → {样本数, ticks}。总调用数从 `ShimRegistry` 的 `call_count` 读（⛔ 不逐次记账）。
    std::map<std::string, ShimStat> shim_by_name;

    void reset() { *this = AotProfile{}; }
};

// 平台取时：x86 = TSC ticks，其它平台 = `steady_clock` 纳秒（口径由 `AotProfile::tsc_ok` 定）。
std::uint64_t profileTicks();
// "自进程启动到现在"的毫秒（Windows 用 `GetProcessTimes`，其它平台退到程序静态初始化时刻）。
double profileProcessMs();
// ticks → ms（`tsc_ok == false` 时 ticks 就是 ns）。
double profileTicksToMs(std::uint64_t ticks, const AotProfile& p);

// =====================================================================================
class AotHost final : public HostServices, public ControlHostSide {
public:
    AotHost() = default;
    // 内含 4 GiB 虚拟预留（`GuestMemory`）与 shim 注册表（互斥量）/ 堆块表 ⇒ 不支持拷贝。
    AotHost(const AotHost&) = delete;
    AotHost& operator=(const AotHost&) = delete;

    // ① 平台 + guest 地址空间：4 GiB 预留（构造时已做，这里失败要能报出来）+ 平台 fault handler
    //    （由 `GuestMemory::reserve` 装）+ `LIFT_SAFE_MEMORY` 同口径 + surface arena + 把**全部**
    //    服务指针接到 cpu 上（`Machine::init` 的 surfaces + `Machine::wire` 的接线）。
    bool init(std::string* err);

    // ①' **S1/S2 的音频配置入口**（`aot_main` 在 `init()` 之后、`run()` 之前调一次）：
    //   `--audio` 的档位 / `--audio-dump` 的路径 / `--sound` 的静音门，全部落到 `AudioEngine`。
    //   ★ 缺省档（三个 flag 都不给）= `configure(Off, "", false)` —— 与 `init()` 里那次逐位相同
    //     ⇒ 默认档行为 / 输出逐字不变。
    //   ★ 值域校验**不在这里**：那是 `runtime/audio_policy.h` 的 `parseAudioMode`（唯一一份），
    //     这里只收已经合法的 `AudioMode`。
    bool configureAudio(AudioMode mode, const std::string& dump_path, bool sound, std::string* err);

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

    // ④' 显示 / GPU 后端：**在第一条 guest 指令之前**接到宿主服务上（真源 `Machine::setGpuRaster`）。
    //    ★ 语义与 `Machine::gpuRaster()` 逐字相同（`machine.cpp:3311-3315`）：显式设置的指针优先，
    //      否则取 presenter 的 sink。`nullptr` = 没有 GPU 光栅后端（3D 一个像素都不画 —— 那是
    //      `d3d9_pipeline.cpp:3983` 的拒批，不是"画得不一样"）。
    //    ★ `aot_main.cpp` 两个档位（headless / window）都设：解释器的 `--present headless` 也建
    //      **隐藏窗口 + GPU device**（`main.cpp:724-726`）⇒ 两臂的像素路径才会是同一个。
    void setGpuRaster(GpuRaster* raster) { gpu_raster_ = raster; }

    // ④'' 控制层端点（`--control-root`；缺省 nullptr ⇒ 块边界的闸门连原子读都不付）。
    //   所有权**不在这里**（`Control` 由 `aot_main.cpp` 持有）⇒ 与 `gpu_raster_` 同形的非拥有指针。
    //   ⛔ 只有主线程调它（`run()` 的块边界汇点在主线程）。
    void setControl(Control* control) { control_ = control; }
    Control* control() { return control_; }
    // ★ 控制层 `hello.present` / `sample.present` 的事实来源（`windowHidden()` 的判据，⛔ 不由
    //   `--present` 推）。`aot_main` 在**起控制层那一刻**算一次（`auto` 会静默回落隐藏窗）。
    void setPresentState(const std::string& state) { present_state_ = state; }

    // ---- `ControlHostSide`（S2/S3/S4；⛔ 只在主线程被调）----
    void fillControlSnapshot(ControlSnapshot& out) override;
    ControlShotStatus controlScreenshot(const std::string& path, ControlScreenshot& out,
                                        std::string* err) override;
    void controlOnResume() override;
    std::uint64_t controlPresentFrame() const override { return presenter_.frameCount(); }
    // ★ `global-get` / `global-set`：直接转发到 `runtime/script_globals.h` 的 **AGE 引擎层实现**
    //   （控制层只搬 JSON / 值；寻址 + ENC/DEC 不在这里）。只在主线程被调；写的是一个 guest 数据槽，
    //   ⛔ 不写 `cpu_` / `regs_`；下一块自然重新从内存加载 ⇒ 写完立刻对引擎可见。
    GlobalError controlGlobalGet(GlobalKind kind, std::uint32_t id, GlobalValue* out) override;
    GlobalError controlGlobalSet(GlobalKind kind, std::uint32_t id, const GlobalValue& value) override;
    // 宿主时钟（控制层的 freeze/unfreeze 与 `guest_ms` 读数都要它；解释器臂同形 `Machine::clock()`）。
    VirtualClock& clock() { return clock_; }
    const VirtualClock& clock() const { return clock_; }

    // ⑤ 读数（bring-up 结束时的"起点"快照）。
    //   ★ `region_log` = `--region-log`（缺省关）：只闸 `regions   :` 那一整块（28 行），
    //     ⛔ 别的 readout（exe / PE / 导入 / 地址与服务面 / pin / 缺口）一格都不收。
    void printReadout(std::FILE* out, bool region_log) const;
    // ⑥ 跑完的摘要（停因 / EA / insns / frames / 派发表读数）。
    //   ★ `--profile` 的整块读数（`=== profile (sampling: …) ===` + 四张表 + `freq:`）也由它在
    //     **最后**打印 —— 这就是"落 stdout 末尾（进程退出前）"的落点（`aot_main.cpp` 在它之后
    //     只写 stderr / 返回退出码）。
    void printRunSummary(const AotRunResult& r, std::FILE* out) const;

    // ---- 采样式内部 profile（`--profile`）----
    // `run()` 开头调一次：清空上一轮的账 + 跑机器频率校准（忙循环，只在开档时）。
    void beginProfile();
    // 关档时调一次：摘掉 `lift_one` 的采样钩子（⛔ 不给"上一轮开过"留一条还在计时的缝）。
    void stopProfile();
    // `lift_one` 采样钩子的落点（转发见 `aot_host.cpp` 的 `aotLiftOneProfileHook`）。
    void noteProfileLiftOne(const lift::LiftOneProfileSample& s);
    // 表块（`prof_.on == false` 时**一行都不打** ⇒ 关档逐字不变）。
    void printProfile(std::FILE* out) const;
    AotProfile& profile() { return prof_; }
    const AotProfile& profile() const { return prof_; }
    // ---- 按帧的 shim 分桶（`--profile-slow-frames <ms>`；缺省关）----
    // run 结束时打**一次**（⛔ 不每帧打）：`frame / wall_ms / guest insns / top-N shim（名字 + ms + 次数）`。
    // `on == false` 时一行都不打 ⇒ 默认档 / 只给 `--profile` 的档逐字不变。
    void printSlowFrames(std::FILE* out) const;
    FrameBucketProf& frameBucket() { return frame_prof_; }
    const FrameBucketProf& frameBucket() const { return frame_prof_; }
    // shim id → `module!name`（慢帧表的具名行用它；格式与窗口 profile 的具名行一致 ⇒ 两张表的行可直接对照）。
    std::string shimDisplayName(std::uint32_t id) const;

    // ---- 窗口 profile（`profile_begin/end` 驱动；**零新 CLI**）----
    // 开窗：清账、快照 insns/frames/lift_dyn、实测一次取时开销（~10 µs）、打开 `lift_dyn` 计数闸门。
    // ★ 返回 false = **已经有一段窗口开着**：此时一个字节都不动（⛔ 不静默 reset —— 重复 `profile_begin`
    //   在配方里是用户可见的错）。判据的唯一实现在 `AotWindowProf::beginWindow()`（pure，可被 CTest
    //   直接驱动）；这里只做宿主侧快照。
    bool beginWindow(const char* label);
    // 收窗：算两个边沿 sliver、打表（含 `unaccounted` 与 `INVALID` 判定）、关闸门。
    // ★ 未闭合（配方只写 `profile_begin`）时由 `run()` 的收口路径调它 ⇒ ⛔ 不静默丢数据。
    void endWindow();
    // ★ 收窗的**取文本**版本（切片 3）：把 `endWindow()` 的函数体原样做完（关窗 + 快照 + 关闸门），
    //   但**不打印** —— `*text` = `formatWindowReportText` 的全文（⛔ 不在这里拼格式串）。
    //   true = 有一段开着的窗口、已被收口；false = 没有开着的窗口（幂等；一个字节都不动）。
    //   `endWindow()` = 成功则 `fputs(text)` ⇒ stdout **逐字不变**（切片 1 的金样判据继续有效）。
    bool endWindowText(std::string* text);
    // 状态位（`REQ-01M7QF01A00F9946DX0GPH23JQ` 设计第 2 条的只读半边）。
    bool profileOpen() const { return win_.profileOpen(); }
    // ★ 宿主能力表（`REQ-01M7QF01A00F9946DX0GPH23JQ` 切片 3）：`ctx = this`，两个 thunk 见下。
    //   返回**成员**地址（生命周期 = 本宿主）；`aot_main` 在 `--overlay` 时交给 sink。
    //   ⛔ 不要把 ops 建在 `aot_main` 的局部变量里（析构顺序反了：局部先于 host 亡）。
    const ui::UiHostOps* uiHostOps();
    // 窗口标记的落点（`InputPlan::ProfileMarkHook` 的 thunk）：begin ⇒ 开窗，end ⇒ 打表 + 关窗。
    // ★ 静态 + `void*` 上下文 = 与 `InputPlan` 的 C 形状钩子对齐（⛔ 不用 std::function：
    //   那会在每个读点付一次间接跳转，而"缺省零成本"是硬口径）。`aot_main.cpp` 取它的地址。
    static void windowMarkThunk(void* ctx, const char* label, bool begin);
    AotWindowProf& window() { return win_; }
    const AotWindowProf& window() const { return win_; }

    // ---- 读数取用 ----
    const PeImage& image() const { return image_; }
    const GuestLayout& layout() const { return layout_; }
    const ProcessSetup& setup() const { return setup_; }
    const X86& cpu() const { return cpu_; }
    X86& cpu() { return cpu_; }
    const ShimRegistry& shims() const { return shims_; }
    const GuestMemory& mem() const { return mem_; }
    const GuestHeap& heap() const { return heap_; }
    // ★ 帧列表读数源（`game.local_frames()` 的宿主取数口）：`init()` 里 bind `&mem_ / &control_marker_`；
    //   `aot_main` 在 `--overlay` 打开时把它交给 sink（⛔ 不每帧推表 —— 脚本按需查询）。
    ui::UiFrameSource* frameSource() { return &frame_source_; }
    const ui::UiFrameSource* frameSource() const { return &frame_source_; }
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
    // ★ 控制层 Present 闸门的 C 函数指针（`HeadlessPresenter::setPresentGate` 的形状；
    //   `user` = 本宿主）。只在主线程被调；`control_ == nullptr` 时已摘掉，不会进这里。
    static bool presentGateThunk(void* user);
    // ★ 宿主能力表的两个 thunk（`UiHostOps::profile_begin/end` 的形状；`ctx` = 本宿主）。
    //   只在 render（`drawOverlay`，Present 主线程）里被脚本间接调用。
    //   `profile_begin`：已开 ⇒ false + 一行 stderr（与配方路径同样的可见性，⛔ stdout 不动）。
    //   `profile_end`：收窗 → 取文本 → **写系统剪贴板** → 一行 stderr；剪贴板失败只写 `*err`，
    //   `text` **不回滚**（需求单"INVALID 仍复制"）。默认档**不周期输出**（只在失败 / 拒绝时一行）。
    static bool uiProfileBeginThunk(void* ctx, const char* label, std::string* err);
    static bool uiProfileEndThunk(void* ctx, std::string* text, std::string* err);
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
    // ★ 收的是 **id + 1**（0 = 桩 EA 不在注册表）而不是 `const ShimDef*`：`lift_shim` 里可能
    //   `addRuntime`（COM 对象创建）⇒ `defs_` 扩容 ⇒ 调用前取到的指针会悬垂（`shims.h` 的警告）。
    //   这**不是**行为改动：`ShimDef::id` 就是下标，`byId()` 与调用前的 `def` 指向同一条。
    void classifyShimStop(std::uint32_t def_id_plus_one, AotRunResult& r);
    // 宿主置停的三分类（真源 `machine.cpp:75-80` 的 `hostStopReason`；那份是内部链接，抄判据不抄代码）。
    static AotStop hostStopReason(const X86& cpu);
    // `regs` ↔ `cpu` 的同步（lifted 侧的状态真源是 `regs`；宿主侧逻辑读 `cpu`）。
    void syncCpuFromRegs();
    void syncRegsFromCpu();
    // ★ 音频 feeder 的 guest 执行器 thunk（`AudioEngine::FeederGuestRunner` 的形状）：
    //   `ctx` = 本宿主；把 `entry/return` 转给 `aot::runAotFeederGuest`（**IR 派发**，⛔ 不走解释器）。
    //   契约与语义见 `src/aot/aot_feeder.h`；⛔ 不要在这里写 guest 语义。
    static bool feederRunnerThunk(void* ctx, X86& cpu, std::uint32_t entry_ea, std::uint32_t return_ea,
                                  std::string* err);

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
    // ★ guest `CRITICAL_SECTION` ↔ 宿主递归锁（与解释器 `Machine` 的 `critical_sections_` 同形）。
    //   真 feeder 线程的**硬前提**：没有它，`guestTryEnterCriticalSectionAs` 会走"guest 字段当锁"
    //   的退化路径（争用时返回**非空 err** ⇒ `AudioEngine::feederEvent` 当致命 ⇒ 第一次并发就打死
    //   feeder；guest 自己的 Enter/Leave 也没有真正互斥）。`init()` 里接 `cpu_.cs_table = &cs_`。
    CriticalSectionTable cs_;
    VirtualClock clock_;
    ComRegistry com_;
    GuestCallStack calls_;
    WindowManager windows_;
    FontEngine fonts_;
    AudioEngine audio_;
    // ★ **S3 泵闸门**（`runtime/audio_pump_gate.h` 的纯状态机，唯一一份）：块边界"该不该调
    //   `audio_.pump()`" + 节流点 + **真调了几次**。`run()` 里那个 `if (audio_pump_.due(…))` 是
    //   AOT 臂上 `AudioEngine::pump()` 的**唯一调用点**；off 档 `due()` 恒假 ⇒ `calls()` 恒 0
    //   （"off 档不进 pump"的机械判据，读数见 `printRunSummary` 的 `audio-pump:` 行 /
    //   `LIFT_AOT_AUDIO_TRACE`）。⚠ 它**不是**第二份音频语义：判据 / 记账只有这一份。
    AudioPumpGate audio_pump_;
    SurfaceArena surfaces_;
    // ★ 宿主能力表（`host.profileBegin/End` 的唯一来源；切片 3）。`ctx = this`，两个 thunk 在 `.cpp`。
    //   ★ **声明在 `presenter_` 之前** = 析构**晚于** `presenter_`：sink 由 `presenter_` 拥有，
    //     而 sink 只存本表的**非 owning 指针** ⇒ "sink 先于 ops 析构"由成员次序保证（不是靠注释）。
    //     ⛔ 不要把它挪到 `presenter_` 之后（那样 ops 的存储先于 sink 结束，未来有人在 sink
    //     析构里读 ops 就是悬垂）。
    ui::UiHostOps ui_host_ops_{};
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
    // ★ 控制层端点（非拥有；缺省 nullptr = 没有控制层 ⇒ 块边界只有一次判空）。
    Control* control_ = nullptr;
    // 控制层快照的脚本身份采样器（有状态：名字解析缓存 / 一次告警去重；只有 `sample` 会驱动它）。
    MarkerSampler control_marker_;
    // ★ 帧列表读数源（`game.local_frames()`；报告的最小版）。`init()` 里 bind `&mem_ / &control_marker_`。
    //   ⛔ 它不改变 `control_marker_` 的既有采样点 / 计数器口径（只共用名字解析器）。
    EngineFrameSource frame_source_;
    // `visible` | `hidden`（连接时的事实；`setPresentState` 写一次）。
    std::string present_state_ = "hidden";
    // ★ 控制层 `quit` 的置停标志（`serve()` 也直接返回 true；这个 bool 是"下单点"的事实面）：
    //   与 `AotRunOptions::stop_requested`（`--input-plan` 的 quit）**分开** —— 两条来源各自可归因。
    bool control_stop_ = false;
    // SEH 展开**不许跨过宿主 guest 回调帧**的判据（真源 `machine.cpp:2208-2236`）：
    // 0 = 不在任何回调里；非 0 = 最内层回调帧的返回槽 EA。影子栈那块是 profiler，⛔ 不复刻。
    std::uint32_t seh_guest_boundary_ = 0;
    // 三个合成 EA 的命中数（读数用：0 = 本轮根本没撞到 —— ⛔ 不拿"实现了"冒充"跑过了"）。
    std::uint64_t timerFires_ = 0;   // 定时器回调铺帧次数
    std::uint64_t callResumes_ = 0;  // guest 回调续延次数
    // ★ `[aot] 定时器 … NULL ⇒ 丢弃` 的**节流到第一次**（按轮重置；口径同 `present.cpp:43` 的
    //   `[present] sink 失败`）：周期定时器的回调为 NULL 时每次到期都会走到那条分支 ⇒ 不节流就是刷屏。
    bool timer_null_warned_ = false;

    // ---- 采样式内部 profile（`--profile`；`on == false` 时所有字段恒为初值、零成本）----
    AotProfile prof_;

    // ---- 窗口 profile（`profile_begin/end` 驱动；`on == false` 时一个取时都不做）----
    AotWindowProf win_;
    // ---- 按帧 shim 分桶（`--profile-slow-frames`；`on == false` 时一个取时都不做、一行都不打）----
    FrameBucketProf frame_prof_;
};

}  // namespace aot
}  // namespace lift
