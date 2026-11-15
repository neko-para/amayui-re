// emulator-lift · 宿主服务接口（shim 依赖的是**接口**，不是解释模型）
//
// ★ 为什么存在（父级裁决 D4 / `.tmp/aot/shim-eval.md`）：
//   解释器臂把 shim 要用的那几份状态**放在 `Machine` 上**；AOT 臂**不构造 `Machine`**。
//   为了不复制那一层 shim（21k 行），把这些状态抽成一个接口，`X86::host` 持一个指针：
//   解释器填 `Machine`（`Machine : public HostServices`），AOT 以后填它自己那份装配。
//   ★ 访问器**沿用 `Machine` 的同名**（`surfaces()` / `presenter()` / `gpuRaster()` / `seh()` /
//     `decoder()` / `closeConfirmPolicy()`）⇒ runtime 里 116 处 `m->xxx()` 一行都不用改，
//     只换"m 从哪来"（`machineOf(cpu)` → `hostOf(cpu)`，见 `runtime/x86.h`）。
//   `handles` / `lst` **不在这里**：它们已经在 `X86` 上（`cpu.handles` / `cpu.lst`）。
//
// ⛔ 这个接口里**不许**加"我是解释器还是 AOT"这类布尔：`hasNestedGuestExecutor()` 是唯一
//   与执行模型的接缝（"有没有一个 run 循环会认领 native override 铺的 guest 调用帧"），
//   它由 `native_image_upload.cpp` 的**响亮停机**分支消费 —— 不是给 shim 分叉用的。
//   （`ownsCpu()` 也**不是**那类布尔：它问的是"这份 `X86` 是谁的"，两条臂同样要按它拒掉
//     别人的副本。）
#pragma once

#include <cstdint>

#include "runtime/message_box.h"

namespace lift {

class SurfaceArena;
class HeadlessPresenter;
class GpuRaster;
class Seh;
class Decoder;
struct X86;

// ★ 一份**不拥有对象**的服务视图：实现方（`Machine` / AOT 宿主）自己拥有状态对象，
//   这里只给 shim 侧的取用面。生命周期由实现方保证（`cpu.host` 在 `Machine::wire()` 里接上、
//   在 `~Machine` 开头摘掉）。
class HostServices {
public:
    virtual ~HostServices() = default;

    // ---- 状态对象（返回引用；调用点必须先确认这台宿主装配了它们）----
    virtual SurfaceArena& surfaces() = 0;
    virtual const SurfaceArena& surfaces() const = 0;
    virtual HeadlessPresenter& presenter() = 0;
    virtual const HeadlessPresenter& presenter() const = 0;
    // SEH 派发状态机（`RaiseException` / `RtlUnwind` 只搬参数，派发在块边界）。
    virtual Seh& seh() = 0;
    virtual const Seh& seh() const = 0;
    // 诊断反汇编（`dumpRecentInsns` / `dumpAtEa` 用；⛔ 不是执行路径）。
    virtual Decoder& decoder() = 0;
    // 可选 GPU 光栅化后端。语义与 `Machine::gpuRaster()` 逐字相同：显式 override 优先，
    // 否则取 presenter 的 sink（sink 也没有 ⇒ nullptr）。
    virtual GpuRaster* gpuRaster() = 0;
    virtual const GpuRaster* gpuRaster() const = 0;
    // `--close-confirm`（AGERC `_ShowDialog@12` 的关窗策略；标量，不是对象）。
    virtual CloseConfirmPolicy closeConfirmPolicy() const = 0;

    // ★ 这台宿主能不能执行"从 native override 里铺的嵌套 guest 调用"？
    //   判据 = 有没有一个解释器 run 循环会在块边界认领 `GuestCallStack` 铺的帧。
    //   `Machine` ⇒ true。AOT 宿主 ⇒ false（铺了帧就再也回不来 ⇒ 调用点**响亮停机**，
    //   ⛔ 不静默落回解释器 —— 那正是"静默跑错"的形状）。
    virtual bool hasNestedGuestExecutor() const = 0;

    // ★ **归属**判据：「`cpu` 是不是本宿主装配/拥有的那一份 `X86`」。
    //   为什么不能只看 `hostOf(cpu) != nullptr`：`AudioEngine` 的 feeder 持有一份 `X86` 的
    //   **整份拷贝**（`audio.cpp` 的 `audio_cpu_ = *cpu_`）⇒ `host` / `machine` 都被复制 ⇒
    //   `host != nullptr` 对 feeder 的**私有副本**同样成立。而 feeder 在**另一个线程**上跑
    //   guest 代码，`SurfaceArena` / `GpuRaster` 却没有任何锁（`surface.h` / `gpu_raster.h`）
    //   ⇒ 只靠 `host != nullptr` 放行 = 允许非主线程碰它们（真数据竞争）。
    //   ★ 判据 = `&this->cpu() == &cpu`（`Machine` 的实现）：`host != nullptr && host->ownsCpu(cpu)`
    //     与解耦前那 7 处 `&m->cpu() == &cpu` **逐字等价**，同时保住 null 安全。
    //   ⛔ 不要改成"把 feeder 副本的 `host` 清空"：那会顺手改掉 `_ShowDialog` 在 feeder 上
    //     读到的 `--close-confirm` 策略（今天是真读 Machine 的 policy）。
    virtual bool ownsCpu(const X86& cpu) const = 0;
};

}  // namespace lift
