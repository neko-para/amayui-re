// emulator-lift · Phase-0 令牌读数（`tokenhold` / `holdhist` / `waithist` / `shimmax`）的**唯一打印点**
//
// ★ 为什么抽成共用函数（2026-11-18）：这四行的口径与格式原本只有解释器 `Machine::dumpRunSummary`
//   一份（`machine.cpp:2845-2908`）；AOT 臂的 `AotHost::printRunSummary` 从不打印它们
//   ⇒ 同一台机器上"谁在持令牌做长活"在 AOT 上**没有读数面**（音频 feeder 恰恰只在 AOT 上跑）。
//   两臂各写一份 = 两份口径迟早分叉 ⇒ 抽成这一个 TU，两个调用点都调它。
//
// ★ 开关不在这里：`guestToken().phase0Enabled()` 由**调用方**判（`arm()` 在 `Machine::run` /
//   `AotHost::run` 里、对 `--profile` 与 `LIFT_AUDIO_TRACE` 取或）。本函数只做格式化 ——
//   ⛔ 不要在这里读环境变量（Phase-0 的唯一读点在 `arm()`，见 `guest_token.h`）。
// ★ 全是**计量**，不是判据：数值随机器 / 调度变（`guest_token.h` 的同一句）。
#pragma once

#include <cstdio>
#include <vector>

namespace lift {

struct ShimDef;

// 打印 Phase-0 四段只读读数：
//   · `tokenhold` = 主/其它角色持令牌累计墙钟、占比（`hold / span`）、最长一次不间断持有、段数；
//   · `holdhist`  = 主线程持有段长直方图（<1 / 1–5 / 5–20 / 20–50 / >50 ms）；
//   · `waithist`  = 主 + 其它角色的**等令牌**直方图（同五桶）；
//   · `shimmax`   = 每个 shim 的**最长一次**宿主墙钟，top 8。
// `shims` = `ShimRegistry::all()`（名字与 id 的唯一来源；⛔ 不在这里另建一张表）。
void dumpTokenPhase0(std::FILE* out, const std::vector<ShimDef>& shims);

}  // namespace lift
