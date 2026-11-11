// emulator-lift · Machine：把 guest 内存 / 解码器 / 块缓存 / shim / 时钟 / 堆 / 句柄装成一台机
//
// ★ 这是**唯一的装配点**：main.cpp 与所有 CTest 都只通过 `Machine` 干活。
//   `initBare()`（不载 PE、直接用自造字节）是纯档夹具的入口 —— 判据 A1 走它。
// ★ 执行循环（dispatcher）在 machine.cpp：
//     eip → 块缓存 → 顺序执行 → 控制流只改 eip 并回循环
//   每一步检查 `--until ea` / 最大指令数 / 第一个 shim（D4 的 L1 / L2）。
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "corpus/lst_index.h"
#include "runtime/clock.h"
#include "runtime/audio.h"
#include "runtime/com.h"
#include "runtime/critical_sections.h"
#include "runtime/font.h"
#include "runtime/present.h"
#include "runtime/surface.h"
#include "runtime/decode.h"
#include "runtime/engine_marker.h"
#include "runtime/guest_call.h"
#include "runtime/host_window.h"
#include "runtime/handles.h"
#include "runtime/heap.h"
#include "runtime/image.h"
#include "runtime/input_script.h"
#include "runtime/interaction_graph.h"
#include "runtime/memory.h"
#include "runtime/native_overrides.h"
#include "runtime/seh.h"
#include "runtime/shims.h"
#include "runtime/vfs.h"
#include "runtime/window_profile.h"
#include "runtime/x86.h"

namespace lift {

enum class StopReason : std::uint8_t {
    None = 0,
    UntilEa,       // 到了 --until ea
    MaxInsns,      // 到了 --max-insns
    FirstShim,     // 到了第一次 shim 调用（L1）
    Unimplemented, // 执行到没实现的操作码 / 未实现的导入（响亮失败）
    DecodeError,   // 解码失败（非法字节 / 越界）
    Stopped,       // guest 自己停（hlt / 停机）
    NoCode,        // eip 落在没有代码的地方
    TimeLimit,     // 墙钟上限（--time-limit；探索用，不用于确定性比对）
    // ★ 识别驱动的交互任务**失败**（三种失败：timeout / no-sample / expect_change）。
    //   ⛔ 不进 `ok()`：它必须是非 0 退出（真源 `docs/03-emulator/interaction-model.md` §2）。
    InteractionFailed,
    // ★ 我方 arena / guest 堆不够（`d3dxResourceStop`）。⛔ 不是正常结局、不进 `ok()`。
    //   与下面的 `Trap` **分开**：容量/碎片问题的修法是改容量或回收，补实现修不了它
    //   （BUG-02 已经把"资源耗尽"与"unimplemented"分开，⛔ 不许合并回一个原因）。
    ResourceExhausted,
    // ★ 已注册但方法槽 / 形态没实现（`comUnsupported` / `d3dxTrap` / `d3d9Trap` / `ddrawTrap` /
    //   `dsTrap` / COM 装配错）。⛔ 不是正常结局、不进 `ok()`。
    //   为什么不复用 `Unimplemented`：那条的判据是"注册表里没有 fn"（`had_impl == false`），
    //   而这里 `had_impl == true` —— 导出 / 槽**已注册**，只是方法体是 loud trap。
    Trap,
    // ★ guest 的 SEH / C++ EH 走不完（`RaiseException` / `RtlUnwind` 的派发状态机报的）：
    //   ① `fs:[0]` 链走到底仍没有 handler 接受（= 真机上的"未处理异常"）；
    //   ② 派发本身做不下去（结构铺不下 / fs_base 没装配 / 展开要跨过宿主 guest 回调帧 / 嵌套异常）。
    //   ⛔ 不是正常结局、不进 `ok()`：它**必须**与"跑完了"区分开（旧形态 `unimplemented` 只说明
    //   "宿主缺导入"，而这里导入已经有了，缺的是"这条异常没人接"）。detail 里写清是哪一种。
    UnhandledException,
};
const char* stopReasonName(StopReason r);

struct RunOptions {
    std::uint32_t until_ea = 0;      // 非 0 ⇒ 到达即停
    std::uint64_t max_insns = 0;     // 非 0 ⇒ 执行到该条数即停
    bool stop_on_first_shim = false; // L1
    bool trace_shims = false;        // 打进 trace_out（缺省 stdout；确定性）
    std::FILE* trace_out = nullptr;  // A4 逐字节比对用：两次运行各写一个文件再比（缺省 stdout）
    bool trace_blocks = false;       // 块级 trace（调试）
    // ★ A4 口径：默认虚拟时钟；`real_time=true` 才接宿主时钟 —— 此时逐字节断言必须由调用方跳过并明说。
    bool real_time = false;
    bool trace_inputs = false;       // 打印输入清单（exe 路径串 / 命令行 / 环境块 / 时钟起点）
    // 探索用墙钟上限（秒，0 = 不限）。不是确定性判据：确定性比对用 max_insns / max_frames。
    double time_limit_sec = 0.0;
    // ★ guest 热点 profile（诊断；默认关）：块命中数记在 Block 实体上，跑完按"命中 × 块内指令数"排序。
    //   ⛔ 这不是性能判据本身：A/B 用墙钟，profile 只回答"热点在哪"。
    bool profile = false;
    std::size_t profile_top = 30;
};

// A4 的**输入清单**（plan §5 A4）：这几项一变，执行路径就可能变。
struct TraceInputs {
    std::string exe_path;       // `--exe` 的原始字符串（相对 / 绝对都原样记；宿主侧输入）
    std::string guest_exe_path; // guest 眼里的主模块路径（`<guest 根>\AGE.EXE`）
    std::string guest_cmdline;  // guest 读到的命令行
    std::string environment;    // 环境块（ANSI 视图；分隔符 ';'）
    std::string game_dir;       // `--game-dir` 原始字符串（空 = 没给）
    std::string appdata_dir;    // `--appdata` 原始字符串（空 = 没给）
    std::string overlay_guest;  // VFS overlay 的 guest 侧（空 = 没有 overlay）
    std::string overlay_host;   // VFS overlay 的宿主侧（= 真正在跑的拆壳件）
    std::string agerc_path;     // `--agerc` / 自动发现的原始路径（空 = 没有 AGERC 镜像）
    std::string agerc_sha256;   // AGERC 镜像的 sha256（空 = 没有镜像）
    std::string native_overrides;  // 解析后的 override 集（name@EA/conv/argB；空 = 全关）
    bool skip_logo = false;        // `--skip-logo`（写引擎 show-logo 标志；不是输入路径变更）
    std::uint64_t clock_start_100ns = 0;
    bool real_time = false;
};

struct RunResult {
    StopReason reason = StopReason::None;
    std::uint32_t ea = 0;        // 停下来的 eip
    std::uint64_t insns = 0;
    std::string detail;
    // ★ 运行摘要 `present-tail` 的打印字段（口径：**最后一次 Present 调用 → 本次 run 停**的 guest 指令）。
    //   `any == false`（本轮 run 内一次 Present 都没有）⇒ 摘要**不打**这一行（⛔ 不是"尾段为 0"）。
    //   字段语义 / 确定性边界见 `runtime/present.h` 的 `PresentTail`（⛔ 这里不重复一份）。
    PresentTail present_tail;
    // ★ 这次 `InteractionFailed` 是**marker 超时**（不是 expect-change 的三种失败，也不是交互图失败）。
    //   ⛔ 不新增 `StopReason` 成员：退出码契约里 `InteractionFailed` 已经是 5；这里只做**分流标志**，
    //   让 `main.cpp` 打对的那条 stderr、⛔ 不落到交互图的 `failureReport()` / `.fail.bmp`。
    bool marker_timeout = false;
    // ★ 这次 `InteractionFailed` 是**时间线 0 点没闩锁**（`--anchor-script` 整个 run 都没**观察到**
    //   进入目标脚本）。⛔ 不新增 `StopReason`：退出码契约里 `InteractionFailed` 已经是 5，这里只做
    //   **分流标志**，让 `main.cpp` 打 `[anchor] FAIL origin-missed: …`、⛔ 不落到交互图的
    //   `failureReport()` / `.fail.bmp`（与 `marker_timeout` 同一条理由）。
    bool origin_missed = false;
    // ★ 只有"正常结局"为真。`ResourceExhausted` / `Trap` / `Unimplemented` / `DecodeError` /
    //   `NoCode` / `None` / `InteractionFailed` 都是非 ok ⇒ 退出码非 0（见 `exitCodeForRun`）。
    bool ok() const { return reason == StopReason::UntilEa || reason == StopReason::MaxInsns ||
                             reason == StopReason::FirstShim || reason == StopReason::Stopped ||
                             reason == StopReason::TimeLimit; }
};

// ★ `main` 最后那道兜底退出码（**只**回答"这次 run 本身成不成功"：ok ⇒ 0，否则 2）。
//   ⛔ 不是完整映射：`main.cpp` 先跑几道消费者策略闸门（--assert-guest-token ⇒ 4、
//   --gpu-strict ⇒ 2/3、--interaction-graph 没走完 / 三种失败 ⇒ 5），它们**优先于**本函数。
//   抽成函数是为了让纯档用例能断言"新停机原因 ⇒ 非 0"（真正的黑盒进程退出码需要 CLI 造 OOM，
//   而兄弟单已裁"⛔ 不把夹具造 OOM 做成产品参数"）。
int exitCodeForRun(const RunResult& r);

// ★ 时间线（`--input-script`）的**效果级判据**（`--expect-change-after` / `--expect-change-window`）：
//   "动作发出去" ≠ "guest 看见了"（BUG-04 的点击被吞、BUG-16 的房间「戻る」无反应，都是它漏掉的）。
//   策略：动作被**应用**之后开一个观测窗，要求窗口内 `FrameSampler::changeCount()` 增长。
//   ★ 窗口单位 = **采样数**（一次 Present = 一个样本），⛔ 不是墙钟：`--present window` 是 real-time
//     时钟（不可复现），采样数是"guest 又画了几帧"的确定性计数。
//   ⛔ 缺省关：打开会改变既有脚本的退出码（"点了确实没反应"的已知缺陷会立刻红 ⇒ 需要单独裁决）。
struct ExpectChangePolicy {
    bool enabled = false;
    bool click = false;  // `click`（Kind::Click）
    bool key = false;    // `key_down` / `key_up`
    bool mouse = false;  // `mouse_down` / `mouse_up`（`MouseMove` 不要求变化：hover 本来就可能不改像素）
    // 观测窗长度（采样数；> 0）。窗口里"采样在增长但画面一次都没变" ⇒ not-changed 失败。
    std::uint64_t window_samples = 0;
    std::string text;  // 规范化写法（启动日志 / 失败信息；⛔ 不参与判定）
};

// `--expect-change-after` 的闭集合解析：`click[,key][,mouse]`（逗号分隔、可换序、⛔ 不许重复）。
// 拼错 / 空项 ⇒ false + err（⛔ 不静默退化成"全关"——那正是本单要消灭的静默失败）。
bool parseExpectChangeActions(const std::string& spec, ExpectChangePolicy* out, std::string* err);

// 观测窗的可观测账（守卫 / 运行摘要用；⛔ 不是墙钟）。
struct ExpectChangeStats {
    std::uint64_t windows_opened = 0;
    std::uint64_t windows_satisfied = 0;
    std::uint64_t windows_failed = 0;
};

// ★ 本轮**真实生效**的仪器位（⛔ 不是 CLI 回显：`window-prof` 读的是 `WindowProfiler::armed()`）。
//   运行摘要的 `instr :` 行就是它 —— `docs/03-emulator/measurement.md` M-02 的**口径标签**。
struct InstrumentationState {
    bool profile = false;
    bool frame_trace = false;
    bool window_prof = false;
    bool expect_change = false;
    std::uint64_t expect_windows = 0;  // 本轮真的开了几个观测窗（0 = 没有一个匹配的动作）
};

class Machine {
public:
    Machine();
    ~Machine();
    Machine(const Machine&) = delete;
    Machine& operator=(const Machine&) = delete;

    // 建 guest 地址空间（4 GiB 预留）+ 装 fault 诊断 + 装平台 handler。
    // env: LIFT_SAFE_MEMORY=1 ⇒ safe mode
    bool init(std::string* err);
    bool initialized() const { return initialized_; }

    // PE 载入（含 sha256 校验；`expect_sha256` 非空 ⇒ 必须相等，否则拒绝启动）
    bool loadImage(const std::string& exe_path, std::string* err);
    bool loadLst(const std::string& lst_path, std::string* err);
    // 栈 / TEB / PEB / TLS 数组 + 命令行；`entry_override` 非 0 覆盖入口
    bool prepare(std::uint32_t entry_override, std::string* err);

    // ★ AGERC.DLL 作为**真实 guest 模块**装载（用户 2026-11-04 改口径）：
    //   `loadPeFile` → 选一个不与 image/heap/stack/TEB/shim/va_cursor 冲突的 guest base
    //   → `mapPeImageAt` → 把镜像指针 / 真实 ImageBase 交给 `cpu.shim`（LoadLibrary 时才置 loaded）。
    //   ⚠ 它**不等于** guest 调过 LoadLibrary（后者是 `cpu.shim.agerc_loaded`）。
    //   失败 ⇒ false + err（err 点明是哪一步：读/解析 PE、选基址、映射）。
    bool loadAgercModule(const std::string& path, std::string* err);

    // ---- 纯档夹具入口：不载 PE，直接把机器码放进 guest ----
    bool initBare(std::string* err);
    bool placeCode(std::uint32_t ea, const void* bytes, std::size_t n, std::string* err);
    bool placeData(std::uint32_t ea, const void* bytes, std::size_t n, std::string* err);
    // 给夹具用的平坦栈（1 MiB @ layout.stack_top 下）+ 返回地址 0xFFFFFFFF（执行到就停）
    bool prepareStack(std::string* err);

    RunResult run(const RunOptions& opts);

    // ★ guest 函数级 native override（试点）：按名字从 `.lst` 解析 EA（`_memcpy` / `_memset` / …），
    //   列表用逗号分隔；`all` = 已验证项；`-名字` = 排除（排除优先，如 `all,-sub_48B090`）。
    //   失败 ⇒ false + err（⛔ 不静默跳过）；必须在 `loadLst()` 之后调用。默认空 = 全关。
    bool configureNativeOverrides(const std::string& list, std::string* err);
    NativeOverrideTable& overrides() { return overrides_; }
    const NativeOverrideTable& overrides() const { return overrides_; }
    // ★ 方案 D（docs/thread-model-feasibility.md §5.1）：音频设备模型 + 喂料泵。
    //   `mode=Off` = 旧 silent 口径；`Sdl` = 真设备（默认 CLI）；`Virtual` = 确定性时钟（CTest/dump）。
    //   ★ `sound=false`（缺省）= 静音：全流程照跑，只在最终交付点把缓冲置 0（`--sound` 才出声）。
    bool configureAudio(AudioMode mode, const std::string& dump_path, bool sound, std::string* err);
    AudioEngine& audio() { return audio_; }
    const AudioEngine& audio() const { return audio_; }
    // ★ 段级 override（函数中段；固定 EA）—— 与函数级分开两张表，语义不同：
    //   函数级 = "call 目标 + 返回地址 + 只写 EAX"；段级 = "block 入口 + 跳到出口 + 可写任意寄存器"。
    NativeBlockTable& blockOverrides() { return block_overrides_; }
    const NativeBlockTable& blockOverrides() const { return block_overrides_; }

    // ★ `--skip-logo`：**不跳过 boot**，只在 T_Command 构造完成、指针发布前把引擎自己的
    //   show-logo 标志（T_Command + 0x5EB5C）写 0；根脚本的 0x130 因而走"不 call-script LOGO"分支。
    //   钩子点见 machine.cpp（`kShowLogoInitEa`）；⛔ 不是"直接跳到 TITLE"的宿主捷径。
    void setSkipLogo(bool on) { skip_logo_ = on; }
    bool skipLogo() const { return skip_logo_; }
    bool skipLogoApplied() const { return skip_logo_applied_; }

    // ★ 指令式输入时间线（`--input-script`）：见 `runtime/input_script.h`。
    //   加载失败 ⇒ false + err（带行号）；⛔ 不静默跳过。可以在 run 之前 / 两次 run 之间替换。
    bool loadInputScript(const std::string& path, std::string* err);
    // 直接给文本（测试 / 内嵌用）：与 `loadInputScript` 走同一个解析器（⛔ 没有第二套语法）。
    bool loadInputScriptText(const std::string& text, const std::string& origin, std::string* err);
    InputScript& inputScript() { return input_script_; }
    const InputScript& inputScript() const { return input_script_; }
    // ★ 帧停滞兜底阈值（`--input-stall-ms`；0 = 关闭）。必须在 `run()` 之前设。
    // ★ 这三个旋钮**同时**作用到两条通道（时间线 + 交互图的 actuator）：图把动作追加进自己的
    //   actuator，用户传 `--input-max-hold` / `--input-stall-ms` 时**必须**也生效 ——
    //   否则那三个 flag 对图静默无效（复审发现）。
    void setInputStallMs(std::uint64_t ms) {
        input_script_.setDefaultStallMs(ms);
        interaction_.actuator().setDefaultStallMs(ms);
    }
    // 还没出现第一帧时的宽限（`--input-stall-boot-ms`；boot 期间没有 Present 不算停滞）。
    void setInputStallBootGraceMs(std::uint64_t ms) {
        input_script_.setDefaultBootGraceMs(ms);
        interaction_.actuator().setDefaultBootGraceMs(ms);
    }
    // ★ 自动 hold 的上限（`--input-max-hold`，guest ms；0 = 不设限）。必须在 `run()` 之前设。
    //   到上限仍未采样 ⇒ 抬起 + 告警（见 `runtime/input_script.h`）；⛔ 不挂死。
    void setInputMaxHoldMs(std::uint64_t ms) {
        input_script_.setMaxAutoHoldMs(ms);
        interaction_.actuator().setMaxAutoHoldMs(ms);
    }
    // ★ marker 超时（`--input-marker-timeout-ms`，**宿主墙钟** ms；0 = 不设限）。只作用到时间线：
    //   交互图的 actuator 没有 marker 动作源（图生成的是 `at frame`）。必须在 `run()` 之前设。
    void setInputMarkerTimeoutMs(std::uint64_t ms) { input_script_.setMarkerTimeoutMs(ms); }
    // ★ 在 VM 循环头的**块入口**采一次样并喂给时间线（`Machine::run` 的钩子调它；纯档夹具也直接调）。
    //   返回 true = 这次产生了边沿（有新事件）。⛔ 它**不是**通用块边界采样入口：调用点必须已经
    //   确认 `cpu_.eip` 是 `kMarkerLoopHeadEa` / `kMarkerPumpReturnEa`（见 `runtime/engine_marker.h`）。
    bool sampleMarker();
    const MarkerSample& lastMarkerSample() const { return last_marker_sample_; }
    const MarkerSampler& markerSampler() const { return marker_sampler_; }
    // 每个块边界推进一次注入时间线（`Machine::run` 内部调用；`GetMessageA` 阻塞时的宿主事件泵
    // 也调用它，否则 guest 卡在等消息时脚本永远不触发）。
    void tickInputScript(X86& cpu);

    // ★ 识别驱动的交互任务（`--interaction-graph`）：见 `runtime/interaction_graph.h`，
    //   真源 `docs/03-emulator/interaction-model.md`。与时间线**并存**（真源 §7 第 3 期才合并）。
    bool loadInteractionGraph(const std::string& path, std::string* err);
    // 直接给文本（测试 / 内嵌用）：与 `loadInteractionGraph` 走同一个解析器。
    bool loadInteractionGraphText(const std::string& text, const std::string& origin, std::string* err);
    InteractionGraph& interactionGraph() { return interaction_; }
    const InteractionGraph& interactionGraph() const { return interaction_; }
    // 画面特征采样器：加载图时挂到 presenter 上（没加载图 ⇒ 不采样、零成本）。
    FrameSampler& sampler() { return sampler_; }
    // 每个块边界推进一次交互图（顺序：先图 tick（它 append 动作）再 actuator tick（真正发 WM_*））。
    void tickInteraction(X86& cpu);

    // ★ 时间线的效果级判据（见上面的 `ExpectChangePolicy`）：必须在 `run()` 之前、**在
    //   `loadInputScript*` 之后**配置一次（策略要扫一遍脚本的动作；零匹配 = 拒绝 ⇒ 不许静默开局）。
    //   与 `--interaction-graph` **互斥**（图有自己的 `expect_change` 语义；两套判据同时开 = 两套口径）。
    bool configureExpectChange(const ExpectChangePolicy& p, std::string* err);
    bool expectChangeEnabled() const { return expect_policy_.enabled; }
    const ExpectChangePolicy& expectChangePolicy() const { return expect_policy_; }
    const ExpectChangeStats& expectChangeStats() const { return expect_stats_; }
    // ★ 区间 profiler 的**真实** armed 位（`WindowProfiler::armed()`，不是"脚本里有没有 profile 动作"）：
    //   运行摘要 `instr :` 行与守卫都读它。
    bool profilerArmed() const { return window_prof_.armed(); }
    const InstrumentationState& instrumentation() const { return instrument_; }

    // ★ SEH 派发状态机（`RaiseException` / `RtlUnwind`）：shim 只把参数搬进来，真正的
    //   "铺 handler 帧 → 跑 guest → 收 disposition"在**块边界**（`pumpSeh`）。
    Seh& seh() { return seh_; }
    const Seh& seh() const { return seh_; }

    // ---- 访问器 ----
    GuestMemory& mem() { return mem_; }
    const GuestMemory& mem() const { return mem_; }
    X86& cpu() { return cpu_; }
    const X86& cpu() const { return cpu_; }
    Decoder& decoder() { return decoder_; }
    BlockCache& cache() { return cache_; }
    ShimRegistry& shims() { return shims_; }
    const ShimRegistry& shims() const { return shims_; }
    VirtualClock& clock() { return clock_; }
    GuestHeap& heap() { return heap_; }
    HandleTable& handles() { return handles_; }
    Vfs& vfs() { return vfs_; }
    const Vfs& vfs() const { return vfs_; }
    SurfaceArena& surfaces() { return surfaces_; }
    const SurfaceArena& surfaces() const { return surfaces_; }
    // ★ P2：宿主字体层（FreeType 桥 / 合成字形接缝）；gdi32 的字体 shim 与夹具都从这里取。
    FontEngine& fonts() { return fonts_; }
    const FontEngine& fonts() const { return fonts_; }
    HeadlessPresenter& presenter() { return presenter_; }
    const HeadlessPresenter& presenter() const { return presenter_; }
    // 可选 GPU 光栅化后端（由 PresentSink 或 main 的离屏 raster window 提供）。
    void setGpuRaster(GpuRaster* gpu) { gpu_raster_ = gpu; }
    GpuRaster* gpuRaster();
    const GpuRaster* gpuRaster() const;
    LstIndex& lst() { return lst_; }
    const LstIndex& lst() const { return lst_; }
    const PeImage& image() const { return image_; }
    const PeImage& agercImage() const { return agerc_image_; }
    const std::string& agercPath() const { return agerc_path_; }
    std::uint32_t agercBase() const { return agerc_base_; }
    bool agercModuleLoaded() const { return agerc_loaded_; }
    const ProcessSetup& setup() const { return setup_; }
    GuestLayout& layout() { return layout_; }
    const GuestLayout& layout() const { return layout_; }
    bool imageLoaded() const { return image_loaded_; }

    // 诊断
    void reportBaseGaps(std::FILE* out) const;
    // A4 输入清单（plan §5 A4）：trace 的"输入"必须显式 —— 换个 --exe 路径写法就是另一条执行路径。
    TraceInputs traceInputs() const;
    void dumpTraceInputs(std::FILE* out) const;
    std::uint64_t traceInputsHash() const;  // 输入归一化后的指纹（两次运行必须相同）
    // 机器可读的缺口/覆盖快照（给 `tools/lift.mjs report` 用；⛔ 人读输出不要靠它）
    void reportCoverageJson(std::FILE* out) const;
    void dumpRunSummary(const RunResult& r, std::FILE* out) const;
    // guest 热点（块级 + 函数聚合）。没有 `.lst` 时函数列是 `?`（⛔ 不猜）。
    void dumpProfile(std::FILE* out, std::size_t top) const;

private:
    void wire();
    // ★ 区间 profiler（`profile_begin` / `profile_end`；见 runtime/window_profile.h）。
    //   窗口边界 = 脚本动作被触发的那一个 **block 边界**（帧内也能切：`at insn N`）。
    //   这里负责四件事：① 在迭代边界开/关窗口；② 分段记账；③ 结束就打印一次；④ 未闭合告警收口。
    void applyPendingProfileWindow();
    void closeProfileWindow(const char* why);
    void printWindowReport(const WindowReport& r) const;
    std::uint32_t fnKeyOf(std::uint32_t ea) const;
    // ★ 诊断（`LIFT_FRAME_TRACE=1`）：每次 presenter 帧号变化时打一行"这一帧的墙钟去哪了"。
    //   只读：不改变执行结果；shim 计时由 `shims_.setTiming(true)` 打开。
    void flushFrameTrace();
    // ★ 一份 `InputScript` 的公共推进（时间线与交互图的 actuator **共用**这条路径：
    //   tick / profiler 窗口边界登记 / `quit` 唤醒 WM_QUIT）。⛔ 不要给两条通道各写一遍。
    void pumpScript(InputScript& script, X86& cpu, std::uint64_t wall_ms);
    // ---- 时间线的效果级判据（`--expect-change-*`；见 `ExpectChangePolicy`）----
    //   ★ 开窗**就地**在 `tickInputScript` 里做（⛔ 不延迟到块边界）：`InputScript::tick` 里
    //     `quit` 之后的同 tick 动作不会再被应用，而块边界的 `quit` 分支会先 break ——
    //     延迟开窗会让 `click` + `quit` 静默通过（正是本单要消灭的"发完不管"）。
    struct ExpectWindow {
        std::string action_text;
        std::uint64_t changes_at_start = 0;
        std::uint64_t samples_at_start = 0;
        std::uint64_t start_frame = 0;
        std::uint64_t start_insns = 0;
        std::uint64_t start_guest_ms = 0;
    };
    bool expectWatches(InputScript::Kind kind) const;
    void openExpectWindows(std::size_t applied_before);
    void evaluateExpectWindows();
    void failExpectWindow(const ExpectWindow& w, const char* kind, const char* why);
    std::uint64_t wallMs() const;
    // winmm 定时器：找到期最早的一个并把控制权交给 guest 回调（协作式；见 x86.h 的 GuestTimer）。
    // 返回 true = 已铺好回调帧（调用方应 continue）；false = 没有到期项或铺帧失败（后者设 r）。
    bool fireDueTimer(RunResult& r);
    // ★ SEH 派发（块边界）：① 认领 shim 的交接（`kSehDispatchEa`）；② 若 handler 已返回或
    //   guest 弃帧就推进状态机，并同步影子栈（`WindowProfiler`）。
    //   返回 true = 本次迭代已经铺好 guest 帧（调用方 `continue`）；false = 照常往下，
    //   但 `r.reason != None` 时调用方必须 `break`（响亮停）。
    bool pumpSeh(RunResult& r);
    friend void liftFaultHook(const AccessSite& site);

    GuestMemory mem_;
    Decoder decoder_;
    BlockCache cache_;
    ShimRegistry shims_;
    VirtualClock clock_;
    Vfs vfs_;
    GuestHeap heap_;
    HandleTable handles_;
    GuestCallStack guest_calls_;
    CriticalSectionTable critical_sections_;
    NativeOverrideTable overrides_;
    NativeBlockTable block_overrides_;
    InputScript input_script_;
    // ★ marker 采样器 + 最近一次样本（时间线要身份 / 图里有 `script` 条件 / `LIFT_LOG_MARKERS`
    //   任一成立时才会被 run 钩子调用）。
    MarkerSampler marker_sampler_;
    MarkerSample last_marker_sample_;
    // ★ 诊断（`LIFT_LOG_MARKERS`）：身份**变化**时打一行 `[marker] …`（发现路径：没有它，
    //   写 `script <名字>` 只能靠猜）。**存在即开**（`=0` 也算开 —— 与 `LIFT_FRAME_TRACE` 同口径）。
    //   下面几个字段只是"上一次打过的身份"（稳态不刷屏的判据）。
    bool log_markers_ = false;
    bool marker_logged_identity_ = false;
    std::uint32_t marker_logged_cur_ = 0;
    std::uint32_t marker_logged_id_ = 0;
    bool marker_logged_degraded_ = false;
    std::string marker_logged_name_;
    // ★ 识别驱动的交互任务（`--interaction-graph`）+ 它的画面采样器。
    //   采样器只在这张图被加载时挂到 presenter 上（`loadInteractionGraph`），否则一次也不采样。
    InteractionGraph interaction_;
    FrameSampler sampler_;
    // 图失败 / 完成时投 WM_QUIT 唤醒阻塞中的 guest（只投一次）。
    bool interaction_stop_posted_ = false;
    // ★ 区间 profiler（`runtime/window_profile.h`）：armed = 脚本里有 profile_begin/end 动作。
    //   ⛔ 没有这些动作时它整条路径都是 no-op（两次 steady_clock 也不会有）。
    WindowProfiler window_prof_;
    // ★ SEH / C++ EH 的派发状态机（见 runtime/seh.h）。跨块迭代存活；每轮 `run()` 开头清空。
    Seh seh_;
    // ★ SEH 展开的**宿主 guest 回调边界**（0 = 没有）：某个 guest 回调帧的返回槽 EA
    //   （`GuestCallStack::start` 在那里写 `kContinuationEa`；winmm 定时器回调是 `timer_saved_esp-24`）。
    //   判据：展开落地 `esp >= 这个槽` ⇒ 异常要"从回调里穿出去"——真机上那会穿过宿主 C++ 栈（UB）
    //   ⇒ 本线**响亮失败**（⛔ 不许静默把 `guest_calls_` 清掉继续跑）。
    //   ★ "在回调**里面** catch"（room-exit 的形状：throw 与 catch 同在 `sub_412290` 的帧里）
    //     落地 esp 仍在这个槽之下 ⇒ 放行；这不是"跨帧"。
    std::uint32_t seh_guest_boundary_ = 0;
    bool profile_begin_pending_ = false;
    bool profile_end_pending_ = false;
    std::string profile_begin_label_;
    std::string profile_end_label_;
    // ★ 时间线的效果级判据（`--expect-change-*`）：窗 / 账 / 本轮真实生效的仪器位。
    ExpectChangePolicy expect_policy_;
    std::vector<ExpectWindow> expect_windows_;
    ExpectChangeStats expect_stats_;
    bool expect_failed_ = false;     // 已判死（三种之一）；detail 在 `expect_failure_`
    std::string expect_failure_;     // 进 `RunResult.detail`（main 据此 exit 5）
    InstrumentationState instrument_;  // 运行摘要 `instr :` 行（⛔ 不是 CLI 回显）
    std::chrono::steady_clock::time_point wall_start_{};
    bool wall_start_valid_ = false;
    // ★ 影子调用栈：上一个 block 以 `call` 结束时的返回地址（下一个迭代压帧时用）。
    std::uint32_t pending_call_ret_ = 0;
    WindowManager windows_;
    ComRegistry com_;
    SurfaceArena surfaces_;
    FontEngine fonts_;
    HeadlessPresenter presenter_;
    // ★ 声明在 presenter 之后 ⇒ 析构先于 presenter（AudioEngine 必须在 SDL_Quit 之前关流）。
    AudioEngine audio_;
    GpuRaster* gpu_raster_ = nullptr;
    LstIndex lst_;
    X86 cpu_;
    PeImage image_;
    // AGERC 的真实镜像（节已映射进 guest；`agerc_base_` 是它真实的 ImageBase / HINSTANCE）
    PeImage agerc_image_;
    std::string agerc_path_;
    std::uint32_t agerc_base_ = 0;
    bool agerc_loaded_ = false;  // 镜像已映射（≠ guest 调过 LoadLibrary；后者在 cpu_.shim.agerc_loaded）
    ProcessSetup setup_;
    GuestLayout layout_;
    bool initialized_ = false;
    bool image_loaded_ = false;
    bool bare_ = false;
    bool skip_logo_ = false;
    bool skip_logo_checked_ = false;  // 钩子点已过（成功 / 失败都算，避免反复尝试）
    bool skip_logo_applied_ = false;
    std::uint32_t skip_logo_object_ = 0;
    std::string exe_path_;
    // ★ 诊断状态（`LIFT_FRAME_TRACE=1`）；默认全不参与执行。
    bool frame_trace_ = false;    std::chrono::steady_clock::time_point frame_trace_last_{};
    std::uint64_t frame_trace_last_insns_ = 0;
    std::vector<std::uint64_t> frame_trace_prev_shim_ns_;
    // ★ 逐帧 shim **调用次数**增量（与 ns 同源）：只看墙钟会把"一次很慢"和"很多次很快"混为一谈。
    std::vector<std::uint64_t> frame_trace_prev_shim_calls_;
    std::unordered_map<std::uint32_t, std::uint64_t> frame_trace_prev_block_ns_;
    // ★ 逐帧 GPU 覆盖增量（`LIFT_FRAME_TRACE=1`）：与 summary 的累计值分开记，⛔ 不复位全局账本。
    std::uint64_t frame_trace_prev_gpu_draws_ = 0;
    std::uint64_t frame_trace_prev_gpu_fallback_ = 0;
    std::uint64_t frame_trace_prev_gpu_fallback_px_ = 0;
    std::uint64_t frame_trace_prev_sprite_total_ = 0;
    std::uint64_t frame_trace_prev_sprite_fallback_ = 0;
};

// 纯档夹具用：`Machine` 的全局实例指针（fault 诊断要拿到寄存器与 `.lst`）
Machine* activeMachine();
void setActiveMachine(Machine* m);

// 这台 cpu **所属的** Machine（宿主侧归属，`Machine::wire()` 捕获的 `X86::machine`）。
//   ★ 与 `activeMachine()` 的区别：那是"最后 init 的那台"的进程级全局，可被别的 Machine 合法改写
//     （`~Machine` 也会清空）；本函数回答的是"**谁装配了这台 cpu**"，因此销毁路径可以靠它回收
//     arena —— 哪怕此刻 `activeMachine()` 是另一台（REQ-01M6X4MSCMMFEXYXCSK6V61QR2）。
//   ★ 兜底：没走 `Machine::wire()` 的 X86（直接构造的纯夹具）只有"全局那台且真是这台 cpu"才算数；
//     否则返回 nullptr（宿主不知道 arena 归谁 ⇒ 调用方只能清状态块，⛔ 不许猜）。
Machine* machineOf(const X86& cpu);

}  // namespace lift
