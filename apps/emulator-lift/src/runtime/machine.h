// emulator-lift · Machine：把 guest 内存 / 解码器 / 块缓存 / shim / 时钟 / 堆 / 句柄装成一台机
//
// ★ 这是**唯一的装配点**：main.cpp 与所有 CTest 都只通过 `Machine` 干活。
//   `initBare()`（不载 PE、直接用自造字节）是纯档夹具的入口 —— 判据 A1 走它。
// ★ 执行循环（dispatcher）在 machine.cpp：
//     eip → 块缓存 → 顺序执行 → 控制流只改 eip 并回循环
//   每一步检查 `--until ea` / 最大指令数 / 第一个 shim（D4 的 L1 / L2）。
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "corpus/lst_index.h"
#include "runtime/clock.h"
#include "runtime/com.h"
#include "runtime/font.h"
#include "runtime/present.h"
#include "runtime/surface.h"
#include "runtime/decode.h"
#include "runtime/guest_call.h"
#include "runtime/host_window.h"
#include "runtime/handles.h"
#include "runtime/heap.h"
#include "runtime/image.h"
#include "runtime/memory.h"
#include "runtime/native_overrides.h"
#include "runtime/shims.h"
#include "runtime/vfs.h"
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
    NativeOverrideTable overrides_;
    WindowManager windows_;
    ComRegistry com_;
    SurfaceArena surfaces_;
    FontEngine fonts_;
    HeadlessPresenter presenter_;
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
    std::string exe_path_;
};

// 纯档夹具用：`Machine` 的全局实例指针（fault 诊断要拿到寄存器与 `.lst`）
Machine* activeMachine();
void setActiveMachine(Machine* m);

}  // namespace lift
