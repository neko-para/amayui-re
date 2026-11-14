# emulator-lift · 每块无条件两次 steady_clock ⇒ 默认档白付 ~40–58 ns/块

- id: REQ-01M7CAJHYHWCVF3XQ7NM3X428Y
- type: req
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/insn_cost.cpp#insn-cost/block-clock-gate
- done_reason: 两个 now() 收进同一闸门（machine.cpp: 循环外 block_ns_always = opts.profile；块首 need_block_ns = block_ns_always || window_prof_.armed()，关时不取时钟；块尾第二次 now() 与 b->ns += / addGuest / addBlockInsns 一起进闸门）。消费者判据是**读实现定的**不是照抄：--profile 与 armed() 构成条件；LIFT_FRAME_TRACE 只决定打印、guest= 列的定义就是 --profile 的块时间（改前实测单开 trace 每帧 guest=0.0ms ⇒ 加它只会白付两次时钟并悄悄改口径）；armed() 故意不折进循环外常量（运行期 profile_begin 会重新 arm，冻成常量会静默恒 0）。同进程受控读数（gate-probe，同 insns）：闸门关 40.6-41.7 ns/条 vs 开 87.3-96.5 ⇒ 关掉的 2 次块时钟 ≤93-110 ns/块；600 帧整轮 87.2 s → 67.8/56.0 s（insns 逐字相同 1,012,065,649，但后两次自己差 21% ⇒ 差值里混着负载，⛔ 不全记给闸门）。守卫 insn-cost/block-clock-gate 三臂（无消费者 b.ns==0 / profile>0 / 只开 trace 仍==0 / insns 相等），实测把它改坏会红（49 checks 1 failure）。口径偏差已记录：armed() 每块读一次内联 bool（不取时钟）；顺带发现 overlay_guest_timed_ 是 run 开始快照、运行期 arm 后不更新（既有口径问题，未修）。
- tags: [emulator-lift, performance, instrumentation]

# emulator-lift · 每块**无条件**两次 `steady_clock::now()` ⇒ 默认档白付 ~40–58 ns/块

## 现象（2026-11-14 微基准 `insn-cost` 的副产物，读数见 `OBS-20261114-10`）

`Machine::run` 的块循环里，每块都有**一对无条件**的时钟：

```cpp
const auto block_start = std::chrono::steady_clock::now();     // :1627 —— 无条件
…
const std::uint64_t block_ns = static_cast<std::uint64_t>(      // :1687-1689 —— 又一次 now()
    std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - block_start).count());
if (opts.profile || window_prof_.armed()) b->ns += block_ns;    // :1690 —— 只有累加被门控
window_prof_.addGuest(block_ns);                                // :1691 —— 无门控（形参照收）
```

⇒ 即使**既没 `--profile` 也没开窗口 profiler**，每块仍付两次 `steady_clock::now()`。
本机实测 `steady_clock::now()` = **27–29 ns/次**（负载下；`dispatch-tax` 历史记录是 4.8–10）⇒ **~40–58 ns/块**；
按真实负载 **k = 6.45 条/块**（`OBS-20261114-10`）折算是 **6–9 ns/条**，占 guest-block 42.94 ns/条的 **15–20%**。

## 判据

1. **默认档零成本**：没有 `--profile`、没有窗口 profiler、没有逐帧 trace 时，块循环里**一次 `steady_clock::now()` 都不调**
   （判据：`b->ns` 恒 0 且 `window_prof_.addGuest` 不被调用；实现上把两个 `now()` 收进同一个 `need_block_ns` 闸门）。
2. **有消费者时读数不变**：`--profile` 的 guest 桶 / 窗口 profiler 的 `guest` 桶 / `LIFT_FRAME_TRACE=1` 的逐帧
   `guest=` 三项**语义与量级不变**（同命令前后两次的 `guest` 桶差 ≤10%）。
3. **实测收益**：同一命令（`--present headless --skip-logo --audio off --max-frames 600`，`insns` 应逐字相同）
   前后各一次：整进程墙钟、以及 `insn-cost` 的两点拟合 **b（每块 ns）** 都应下降；把读数写进报告（⛔ 不写成断言）。

## 非目标

- 不改窗口 profiler 的实现（它自己的税只在打开时存在，本来就该付）。
- 不动 `LIFT_FRAME_TRACE` 的语义（它现在依赖 `b->ns`，闸门必须把它算进"需要计时"）。
- 不优化 shim 计时（`shims_.setTiming` 已经门控）。

## 风险

- **漏掉一个消费者** ⇒ 那个消费者拿到恒 0 的 `b->ns`（静默）。落点必须把三个消费者列全：
  `opts.profile` / `window_prof_.armed()` / 逐帧 trace 开关（找到它真正的判据，别凭猜）。
- 逐帧 trace 打开时 `b->ns` 还要给 `frame_trace_prev_block_ns_` 用（`:608-609`），别只护住 `+=`。
