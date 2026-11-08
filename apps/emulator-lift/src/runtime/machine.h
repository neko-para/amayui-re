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
#include "runtime/guest_call.h"
#include "runtime/host_window.h"
#include "runtime/handles.h"
#include "runtime/heap.h"
#include "runtime/image.h"
#include "runtime/input_script.h"
#include "runtime/memory.h"
#include "runtime/native_overrides.h"
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
    bool ok() const { return reason == StopReason::UntilEa || reason == StopReason::MaxInsns ||
                             reason == StopReason::FirstShim || reason == StopReason::Stopped ||
                             reason == StopReason::TimeLimit; }
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
    void setInputStallMs(std::uint64_t ms) { input_script_.setDefaultStallMs(ms); }
    // 还没出现第一帧时的宽限（`--input-stall-boot-ms`；boot 期间没有 Present 不算停滞）。
    void setInputStallBootGraceMs(std::uint64_t ms) { input_script_.setDefaultBootGraceMs(ms); }
    // ★ 自动 hold 的上限（`--input-max-hold`，guest ms；0 = 不设限）。必须在 `run()` 之前设。
    //   到上限仍未采样 ⇒ 抬起 + 告警（见 `runtime/input_script.h`）；⛔ 不挂死。
    void setInputMaxHoldMs(std::uint64_t ms) { input_script_.setMaxAutoHoldMs(ms); }
    // 每个块边界推进一次注入时间线（`Machine::run` 内部调用；`GetMessageA` 阻塞时的宿主事件泵
    // 也调用它，否则 guest 卡在等消息时脚本永远不触发）。
    void tickInputScript(X86& cpu);

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
    // winmm 定时器：找到期最早的一个并把控制权交给 guest 回调（协作式；见 x86.h 的 GuestTimer）。
    // 返回 true = 已铺好回调帧（调用方应 continue）；false = 没有到期项或铺帧失败（后者设 r）。
    bool fireDueTimer(RunResult& r);
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
    // ★ 区间 profiler（`runtime/window_profile.h`）：armed = 脚本里有 profile_begin/end 动作。
    //   ⛔ 没有这些动作时它整条路径都是 no-op（两次 steady_clock 也不会有）。
    WindowProfiler window_prof_;
    bool profile_begin_pending_ = false;
    bool profile_end_pending_ = false;
    std::string profile_begin_label_;
    std::string profile_end_label_;
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

}  // namespace lift
