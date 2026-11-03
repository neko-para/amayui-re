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
#include "runtime/decode.h"
#include "runtime/handles.h"
#include "runtime/heap.h"
#include "runtime/image.h"
#include "runtime/memory.h"
#include "runtime/shims.h"
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
};
const char* stopReasonName(StopReason r);

struct RunOptions {
    std::uint32_t until_ea = 0;      // 非 0 ⇒ 到达即停
    std::uint64_t max_insns = 0;     // 非 0 ⇒ 执行到该条数即停
    bool stop_on_first_shim = false; // L1
    bool trace_shims = false;        // 打进 stdout（确定性）
    bool trace_blocks = false;       // 块级 trace（调试）
};

struct RunResult {
    StopReason reason = StopReason::None;
    std::uint32_t ea = 0;        // 停下来的 eip
    std::uint64_t insns = 0;
    std::string detail;
    bool ok() const { return reason == StopReason::UntilEa || reason == StopReason::MaxInsns ||
                             reason == StopReason::FirstShim || reason == StopReason::Stopped; }
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

    // ---- 纯档夹具入口：不载 PE，直接把机器码放进 guest ----
    bool initBare(std::string* err);
    bool placeCode(std::uint32_t ea, const void* bytes, std::size_t n, std::string* err);
    bool placeData(std::uint32_t ea, const void* bytes, std::size_t n, std::string* err);
    // 给夹具用的平坦栈（1 MiB @ layout.stack_top 下）+ 返回地址 0xFFFFFFFF（执行到就停）
    bool prepareStack(std::string* err);

    RunResult run(const RunOptions& opts);

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
    LstIndex& lst() { return lst_; }
    const LstIndex& lst() const { return lst_; }
    const PeImage& image() const { return image_; }
    const ProcessSetup& setup() const { return setup_; }
    GuestLayout& layout() { return layout_; }
    const GuestLayout& layout() const { return layout_; }
    bool imageLoaded() const { return image_loaded_; }

    // 诊断
    void reportBaseGaps(std::FILE* out) const;
    void dumpRunSummary(const RunResult& r, std::FILE* out) const;

private:
    void wire();
    friend void liftFaultHook(const AccessSite& site);

    GuestMemory mem_;
    Decoder decoder_;
    BlockCache cache_;
    ShimRegistry shims_;
    VirtualClock clock_;
    GuestHeap heap_;
    HandleTable handles_;
    LstIndex lst_;
    X86 cpu_;
    PeImage image_;
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
