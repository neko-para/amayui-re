# AOT 寄存器提升（alloca Cut A）评估：判负，不启用

> 读数日期 2026-11-20 / win32 / `E:\Projects\amayui-re`（main）· 基线 `d1cafce`（note opt-in 默认 OFF）。
> 口径：`RelWithDebInfo`、`split8-O2-lto-abi`、同一闭包（roots 3,277 / entries 35,222 / functions 10,980 /
> blocks 74,502 / guest 358,525）。命令与原始读数见 `.tmp/aot-alloca/report.md`（本轮实验记录，未入库）。

## 0. 一句话

把 9 个 guest 寄存器槽从 `%st` 提升为函数内 `alloca [9 x i32]`（Cut A：只改 load 通道，所有
`flushRegs()` 位置不动）后，**`__text` 只降 0.84%**（12,760,422 → 12,652,774），达不到 ≥2% 的
采纳门槛；墙钟有同向小收益（3/3 有效对：点击后 60 帧窗口中位 **−1.48%**、尖峰帧之和 **−2.91%**），
但在这台机器 <5% 的分辨力之下，不能算赢。

⇒ **不启用（默认 OFF）、不提交代码；Cut B 不做。** 实验 diff 只留在本地
`.tmp/aot-alloca/cutA.patch`，供将来重新评估；主树的 `apps/emulator-lift/**` 未改。

## 1. 做了什么（Cut A）

设计见 `.tmp/aot-alloca/design.md`（612 行）。落地范围：

- `entry:` 发 `%slots = alloca [9 x i32], align 4` + 9 条 slot GEP + `%st → %slots` 的 9 对 init；
- `loadReg` 的值通道读 `%slots`；
- `flushRegs` 的脏槽**双写**（`%slots` 然后 `%st`）；**所有 `flushRegs()` 位置一条不删**；
- 两处调用返回点（`emitCallReturnCheck` / `emitLiftOne`）在 `invalidateRegs()` 之后 eager reinit 全 9 槽；
- 开关 `LIFT_AOT_REGALLOCA`（缺省 OFF）+ `--regalloc-alloca`；OFF 档 8 份 `.ll` sha256 与改前快照逐份相同。

⛔ 未做 Cut B（块级 may-dirty 数据流 / 删块边 flush）；⛔ 未改 `lift_abi.{h,cpp}`；⛔ 无 per-EA 特判。

## 2. 读数

### 2.1 静态

| 项 | OFF | ON | Δ |
|---|---:|---:|---:|
| `alloca [9 x i32]` | 0 | **10,980**（每 `lift_cc_*` 恰好 1） | — |
| IR `%st` load | 174,342 | 160,776（init 98,820 + reinit 61,956） | −7.8% |
| IR `%st` store | 187,604 | 187,604 | 0 |
| asm `(%st)` 操作数（全量 10,980 函数） | 311,991（0.870/guest） | 204,495（**0.570**/guest） | **−34.5%** |
| asm 栈引用 | 32,832 | 59,450 | **+81.1%** |
| callee-saved spill | 3,475 | 8,721 | **+151%**（+0.0146/guest） |
| `__text` | 12,760,422 | 12,652,774 | **−0.84%** |
| `.ll` 行数 / 字节 | 3.80M / 132.6MB | 4.49M / 160.9MB | +18.3% / +21.3% |

`%st` 访问降了 34.5%，但 phi 值大多没有留在寄存器，而是落到栈上（栈引用 +81%、spill +151%）：
成本从 `%st` 搬到了 `%rsp`。逐函数 **4,340 变好 / 2,433 变差 / 4,207 持平**（最差 `lift_cc_4CE040` +1,586）。
⇒ 瓶颈是值跨块的**驻留位置**，不是访存条数；Cut B 预计同样 <2%，所以不做。

### 2.2 语义

- CTest `-R "aot|lift-ir-gen"`：ON 14/14、OFF 14/14（exit 0）。
- fingerprint（boot → 300 帧）：`exit 0` / `frames 300` / `stop @ eip=00498AE4` /
  `arena in_use=108595152 peak=108595152 surfaces=21 heap_blocks=6674` / `table misses=0` 两档逐位相同。
- SAVE05 map-reveal：12 次 run 全停在 `FIELD.BIN:0x00041A5C`，谓词命中点两档一致。
- `lastHash` / `dispatch total` 有抖动，按 `AGENTS.md` §12.6 **不作判据**。

### 2.3 墙钟

`.tmp/aot-alloca/bench-ra.mjs`（场景/谓词/`HEAVY_PATH`/`PREDICATE_TIMEOUT`/`--pairs` 与
`.tmp/note-bench/bench.mjs` 同口径；只换 build knob 与 variant 名）。6 对里 3 对过闸，3 对因
`insns` +4.5…10.7% 被 `MISMATCH` 判死（帮助气泡重路径）。有效 3 对中位：

| 窗口 | Δ(ON−OFF) | 同向 |
|---|---:|---:|
| `w_post`（点击后 60 帧） | **−1.48%** | 3/3 |
| `Σ spikes`（8 个尖峰帧） | **−2.91%** | 3/3 |
| `w_load`（到地图空闲） | −0.10% | 1/3 |

## 3. 裁决

1. **不启用**：`LIFT_AOT_REGALLOCA` 留在实验补丁里，默认 OFF；主树不保留这 165 行。
2. **Cut B 不做**：块级 `dirty_in` 数据流预计再拿 2–3%，但引入"多前驱 ret 漏 store"的静默面；
   本轮 Cut A 已经把 `%st` 访问降到 0.570/guest，仍只换到 −0.84% `__text`，说明继续搬 store
   的收益不足以抵消复杂度。
3. 若将来重开，先换**驻留位置**（例如块级 liveness 直接产出 SSA/phi，而不是先写 `alloca` 再靠
   SROA），并用**全量 asm** 而不是 60 函数抽样下结论（本轮抽样曾给出 −12.6% 的假象）。

## 4. 复现

```bash
# 应用实验补丁（只在需要重测时；主树默认没有它）
git apply .tmp/aot-alloca/cutA.patch
cd apps/emulator-lift
cmake --preset windows-x64 -DLIFT_AOT_REGALLOCA=ON
cmake --build out --target emulator-lift-aot
llvm-size out/emulator-lift-aot.exe
ctest --test-dir out -R "aot|lift-ir-gen" --output-on-failure
# 墙钟
node ../.tmp/aot-alloca/bench-ra.mjs ab --pairs 3 --build
# 收工
git apply -R .tmp/aot-alloca/cutA.patch
```

> `.tmp/aot-alloca/cutA.patch` 是本地实验产物（gitignored）；它不是入库件。
