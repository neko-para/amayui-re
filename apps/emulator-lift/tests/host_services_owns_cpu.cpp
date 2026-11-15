// emulator-lift · `HostServices::ownsCpu` 的**归属**判据（pure 档）
//
// ★ 这条守的是什么（`runtime/host_services.h` 的 `ownsCpu` / `runtime/machine.h` 的实现）：
//   `AudioEngine` 的 feeder 持有一份 `X86` 的**整份拷贝**（`audio.cpp` 的 `audio_cpu_ = *cpu_`）
//   ⇒ `host` / `machine` 两个指针被一并复制 ⇒ **"`host != nullptr`" 对这份私有副本同样成立**。
//   而 feeder 在**另一个线程**上跑 guest 代码，`SurfaceArena` / `GpuRaster` 都**没有锁**
//   ⇒ 7 处 ddraw / d3d9 / pipeline / sprite 的判据里，凡是没有第二道门的，都只靠
//   `host != nullptr && host->ownsCpu(cpu)` 挡住它。
//
// ★ 红了意味着什么（三件事，逐条对应下面的断言组）：
//   ① `ownsCpu` 对**真的**那份 cpu 不再是 true ⇒ 归属判据本身坏了（真 owner 被拒 ⇒ 生产路径全塌）；
//   ② 它对**副本**也变成 true ⇒ 判据退化成"只判 `host` 非空"，**音频 feeder 的私有副本会被放行**，
//      从非主线程碰无锁的 `SurfaceArena` / `GpuRaster`（真数据竞争，且**静默**：没有崩溃、
//      没有日志，只有偶发的像素错乱 / 堆结构损坏）；
//   ③ 真实入口（ddraw 的 `createSurfaceState`，第一句就是 `requireMachine(...)`）对**副本**不再拒绝 ——
//      ②的"判据坏了"还只是接口层面，③证明它**已经漏到调用点**。
//
// ★ 为什么副本用 `lift::X86 copy = m.cpu();` 造：与 `audio.cpp` 的 `audio_cpu_ = *cpu_` **同一条**
//   路径（隐式拷贝构造 / 拷贝赋值），⛔ 不是手搓一个 `host` 指针的假对象 —— 那样会绕开"指针被复制"
//   这个**真正的**成因，变异成 `return true` 也可能抓不住。
// ★ 为什么控制组要"owner 真建出一块 surface"：只断言"副本拿到 0"会被"这条入口根本没实现 /
//   永远返回 0"骗过（假绿）。同一句调用对 owner 必须**成功**，两者一起才说明 0 是**归属**拒的。
// ★ 纯档：`initBare()` 不载 PE、不读 LFS、不碰游戏安装；只额外 init 一块 guest 堆
//   （surface 状态块要放在 guest 堆上，与 `tests/ddraw.cpp` 的夹具同一口径）。
#include <cstddef>
#include <cstdint>
#include <string>

#include "check.h"
#include "runtime/ddraw_surface.h"
#include "runtime/host_services.h"
#include "runtime/machine.h"
#include "runtime/x86.h"

namespace {

constexpr std::uint32_t kHeapBytes = 0x00100000u;  // 1 MiB（同 tests/ddraw.cpp 的夹具）

// 最小机器：`initBare()`（不载 PE）+ guest 堆。
struct Harness {
    lift::Machine m;
    std::string err;

    bool build() {
        if (!m.initBare(&err)) return false;
        if (!m.heap().init(&m.mem(), m.layout().heap_base, kHeapBytes)) {
            err = "guest 堆 init 失败";
            return false;
        }
        return true;
    }
};

}  // namespace

int main() {
    Harness h;
    if (!h.build()) {
        // 建不出机器 = 这台机器跑不了这条（虚拟地址预留失败这类）⇒ 跳过，⛔ 不假绿。
        const std::string why = "Machine::initBare 失败（" + h.err + "）";
        LIFT_SKIP(why.c_str());
    }

    lift::X86& cpu = h.m.cpu();
    lift::HostServices* host = lift::hostOf(cpu);

    // ---- ① 真 owner：`host` 非空，且这台 cpu 就是它装配的那一份 ----
    LIFT_CHECK(host != nullptr);
    if (host != nullptr) {
        LIFT_CHECK(host->ownsCpu(cpu));
        LIFT_CHECK(host == static_cast<lift::HostServices*>(&h.m));  // 解释器臂的装配者就是 Machine 自己
    }

    // ---- ② 副本（= `audio_cpu_ = *cpu_`）：`host` 被复制过去，但归属**不属于**它 ----
    lift::X86 copy = h.m.cpu();
    LIFT_CHECK(copy.host != nullptr);                       // ★ 前提：判据不是"只判非空"
    LIFT_CHECK(lift::hostOf(copy) == copy.host);            // `hostOf` 读的就是 `X86::host`
    if (copy.host != nullptr) {
        LIFT_CHECK(!copy.host->ownsCpu(copy));              // ← 变异成 `return true` 就红在这一行
    }
    if (host != nullptr) {
        LIFT_CHECK(!host->ownsCpu(copy));                   // 从 owner 的视角问同一句：副本不是我的
        LIFT_CHECK(host->ownsCpu(cpu));                     // 同一个宿主对**自己**那份仍答 true（不是恒 false）
    }

    // ---- ③ 真实入口：ddraw 的 `createSurfaceState` 第一句就是 `requireMachine(cpu, &m, why)` ----
    //   控制组（owner）必须**建出**一块 8×8 非 primary surface；实验组（副本）必须被拒（0）。
    //   ⛔ 两者用的是同一个堆 / 同一张 arena / 同一份设备状态 ⇒ "副本拿到 0" 只能是**归属**拒的。
    if (host != nullptr) {
        std::string why_copy;
        const std::uint32_t copy_state =
            lift::ddraw::createSurfaceState(copy, 8, 8, 0u, /*primary=*/false, 0u, &why_copy);
        LIFT_CHECK_EQ_U32(copy_state, 0u);
        LIFT_CHECK(!why_copy.empty());  // 被拒**并**给了原因（⛔ 不是"静默拿到 0"）
        if (copy_state != 0) {
            // 变异态才会走到这里（判据已被弱化）：把误建的那块收回，别把垃圾留在 arena 里。
            lift::ddraw::destroySurfaceState(copy, copy_state);
        }

        //    ★ 控制组的账要收干净：块数回到 baseline ⇒ 这条用例不往 arena 里留垃圾，
        //      也顺带钉住"归属正确的那个 cpu 走的是**同一张** arena"。
        const std::size_t surfaces_before = h.m.surfaces().surfaceCount();
        std::string why_owner;
        const std::uint32_t owner_state =
            lift::ddraw::createSurfaceState(cpu, 8, 8, 0u, /*primary=*/false, 0u, &why_owner);
        LIFT_CHECK(owner_state != 0);
        LIFT_CHECK(why_owner.empty());  // 成功路径不写 why（它的失败文本只在失败分支里）
        if (owner_state != 0) {
            lift::ddraw::destroySurfaceState(cpu, owner_state);
            LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(h.m.surfaces().surfaceCount()),
                              static_cast<std::int64_t>(surfaces_before));
        }
    }

    return lift_test::finish("lift-host-services-owns-cpu");
}
