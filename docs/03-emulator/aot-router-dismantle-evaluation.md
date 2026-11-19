# AOT router “拆掉 router”评估：no-go（性能上不拆）

> 读数日期 2026-11-20 / win32 / main `f785157`（note OFF；alloca 已回退）。
> 证据与命令见 `.tmp/aot-router/dismantle.md`（只读分析；未改 tracked / 未构建）。
> 口径：`apps/emulator-lift/out` 当前 OFF 生成物 + `llvm-size -A` + `emulator-lift-aot.map` + 300 帧 profile。

## 0. 一句话

**作为性能方案，不拆。** 拆 router 的三种具体形状里：

- (a) 单入口 component 去 `switch`：只值**代码体积清理**（−61…84 KB / −0.5…0.7%），动态 **<0.1%**；
- (b) TB 风格（每 guest 基本块一个函数 + 块指针表）：**+2.5…4.0 MB / +20…31%**，而热函数
  `4E7617` / `4E7530` / `48B490` 全是 1 块 ⇒ 拆了只多 call/ret，**no-go**；
- (c) 保留 component、把已知 `ret i32 <EA>` 改成直接 `tail call`/链化：**同分量 `bb_exit` 直连是唯一
  值得做的小实验**（1,891 个，−9.5…15 KB，能消掉这些 trampoline 派发），但收益是 KB 级，不是墙钟级。

⇒ 真正的成本不是 `switch`，是 **52.0M 次/300 帧的 trampoline round-trip** + **18,494 个 dispatch call**
（5,637/10,980 个 component 不是 structured）。要拿回来，得提高**结构化覆盖率 / 尾调用**，不是删
`switch` 或按块拆函数。

## 1. 体积与入口面（已核）

| 量 | 读数 |
|---|---:|
| 最终 `.text` | **12,760,422 B** |
| `lift_cc_*` 合计 | **8,916,576 B（69.9%）** |
| `lift_cc_*` 函数 | **10,980** = 5,590 单入口（1,248,480 B，223.3 B/函数）+ 5,390 多入口（7,668,096 B，1,422.7 B/函数） |
| 单入口样本 `cmp`+`jne`+`ud2` | 11–15 B/函数 ⇒ 5,590 个约 **61–84 KB**（−0.5…0.7%） |
| arm64 快照 `switch` 比较 / `brk` | 58,521（2.34%）/ 10,254（0.41%） |
| profile 前 20 派发采样中单入口占比 | 41.5%，但每次只省 2–3 条指令 ⇒ **<0.1% 墙钟** |

## 2. 三种形状的账

### (a) 单入口去 switch

删 `switch` 只省入口的 `cmp/jne/ud2`；函数体、prologue、块代码都不变。静态 61–84 KB，
动态 <0.1%。⇒ **只值当代码体积清理**，不是性能优化。若做，判据应是 `--verify-router-shape`
（单入口无 switch、表中恰有 root；多入口 case 集合 == 表项子集）+ 指纹，不用墙钟。

### (b) TB 风格（每块一个函数 + 块指针表）

- 74,502 个 guest 块 vs 10,980 个 component；66.1% 的块 ≤4 guest，平均 4.80 guest/块；
- 每块 prologue 20–30 B ⇒ **+2.5…4.0 MB（+20…31%）**；
- 热函数 1 块 ⇒ 拆了只多一次 call/ret，没有派发收益；
- 还要重做块指针表 / 入口 EA / pin / 返回地址。

⇒ **no-go**；这是“把 4.8 条 guest 的块做成函数”的结构性代价。

### (c) 已知 `ret` 直连 / 尾调用

- 当前 `.ll` 有 **1,891 个** `bb_exit_<E>: ret i32 E`，且 E 是本函数 case；
- profile 前 20 有 4 个同分量目标占 **7.2%** 派发采样；
- 改法：同分量 `bb_exit` 直接 `br`/`tail call` 到目标块，不写 `ret i32 E` 再回 trampoline；
- 静态估计 **−9.5…15 KB**；动态只影响这 4 个目标，**<1%** 量级。

⇒ 唯一值得做的小实验；但它不是“拆 router”，只是去掉同分量出口的一次 round-trip。若要
用它证明方向，判据应是：`ret i32 <same-case>` 1,891→0、`dispatch`/`lookups` 次数下降、
boot 300 / SAVE05 的 `table misses=0` / `stop @ 00498AE4` / arena 逐位相同；墙钟不作判据。

## 3. 真正成本在哪

300 帧基线：`lift_dyn lookups=16,450,343`、`dispatch shim=1,432,759`、trampoline
`lookups=52,024,536`、`dispatch call=18,494`；5,637/10,980 个 component 不是 structured。
⇒ 成本在**每次跨 component 回 trampoline 再查表**，以及非结构化单元的 dispatch call；
`switch` 本身只占 2.34% 静态、动态更小。拆 `switch` / 按块拆函数都治不到这个病。

## 4. 裁决

- **不拆 router**（性能上）。保留 component + `switch` 作为静态 AOT 的代码体积/共享 prologue 折中。
- 不做 (b)；不把 (a) 当性能优化（如要做，只作为独立的代码体积清理）。
- (c1) 同分量 `bb_exit` 直连可作为后续小实验，但预期 KB 级、墙钟不可判；不阻塞当前工作。
- 若要继续追 router 成本，下一步应是**结构化覆盖率 / 尾调用 / 直接链化**，而不是删表项或拆函数。
