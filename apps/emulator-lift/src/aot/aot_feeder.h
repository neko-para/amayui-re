// emulator-lift · AOT 臂的**音频 feeder guest 执行器**（IR 派发；AOT 执行来源只有 IR + shim）
//
// ★ 为什么需要它：音频 feeder 是**真宿主线程**（`AudioEngine` 的 thread 档），它要在自己线程上
//   执行 guest 的 `sub_4B6C30`（Ogg 解码 + 混音）。AOT 臂的执行来源**只有**构建期 lift 出来的 IR
//   与 shim（`AGENTS.md` §12.8 第 1 条）⇒ ⛔ 不能复用解释器的块缓存 `runFeederGuest`：
//   解释器臂在**最宽松的宿主加速**下实测一刀 fill 也要 360–460 ms（AOT 臂没有那套东西：
//   "某 EA 用 C++ 重实现"已于 2026-11-16 整条删除）⇒ 很可能 > 500 ms（quarter）⇒ 直接欠料。
//   D 泵实测同一段 IR 执行只要 ~14 ms。
//
// ★ 与主循环（`aot_run.cpp` 的 `AotHost::run`）**有意不同**的三处（⛔ 不要"顺手对齐"）：
//   ① 不 `syncCpuFromRegs/syncRegsFromCpu`：这里 `LiftRegs st` 就是唯一真源，`X86` 只是侧对象
//      （`lift_one` / `lift_shim_id` 自己会做三件套搬运）；
//   ② **不过块边界泵**：定时器 / SEH 派发 / 控制层 / Present / 帧数闸门都是主线程的东西；
//      feeder 只执行一段纯计算（引擎在 fill 前后自己管 CS / 在飞计数 / 上下文）；
//   ③ `lookupUncounted`：不写 trampoline 的派发账（`DispatchStats` 现在是每线程一份，见
//      `aot_dispatch.cpp`）。`lift_dyn` 的计数仍会发生（那是生成代码自己调的）。
//
// ★ 返回哨兵 `0x7FFEF00C`：主循环**不认**它（`pumpSyntheticEas` 只认 0x7FFEF008 / 0x7FFEF004 /
//   SEH）⇒ 必须由本循环在**查表之前**自己判（解释器版在 `audio.cpp` 的 `runFeederGuest` 第一句）。
//   它也不在桩页 `[0x7FFE0000,0x7FFE8000)` 里（`image.h`）。
//
// ★ 线程边界：本函数跑在 AudioEngine 的 feeder 线程上。它**只**碰传进来的 `cpu`（feeder 私有的
//   `X86` 副本）、`mem`（数据面）、`shims`（容器访问只在 `ShimRegistry::invoke` 里、持 guest 令牌）。
//   ⛔ 不碰 BlockCache（`cpu.cache` 在 AOT feeder 上是 nullptr）：桩 EA 走 `byStubEa` 的无锁快路径，
//   再用**已解析的 id** 调 `lift_shim_id`（`id_plus_one != 0` ⇒ 跳过 `cache->peek`）。
#pragma once

#include <cstdint>
#include <string>

namespace lift {

struct X86;
class GuestMemory;
class ShimRegistry;

namespace aot {

// 在 `cpu` 上从 `entry_ea` 执行到 `return_ea`（= `kFeederReturnEa`）。
// 返回 true = 正常跑到哨兵；false + `*err` = guest 停机 / 派发失败 / 桩 EA 不在注册表
//   （调用方 `AudioEngine::feederEvent` 会置 `feeder_error_` 并停 —— 与解释器路径同一条失败面）。
// ★ `cpu` 必须是 **feeder 私有副本**（`audio_cpu_`）；⛔ 不要传主线程的 `cpu_`。
bool runAotFeederGuest(X86& cpu, GuestMemory& mem, const ShimRegistry& shims,
                       std::uint32_t entry_ea, std::uint32_t return_ea, std::string* err);

}  // namespace aot
}  // namespace lift
