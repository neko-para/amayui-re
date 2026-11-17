# AOT LTO 默认开启（属性对齐后 FELLOW init 窗口 −12.61%）

- id: REQ-01M7M1BSZD1HTGQW1M6PENE2TB
- type: spike
- status: done
- parent: REQ-01M7JHSEZVMYB4XHEN32V7V6HJ
- done_reason: 属性对齐后 LTO 真的内联了 tick/note：tick 站点 −86.4%、note −100%，FELLOW init 墙钟中位 −12.61%（5/5 对全负，指纹 10/10 相同）；代价 __text +3.14%、AOT 链接 0.16 s→37.4 s/3.0 GB ⇒ mac 默认开 LIFT_AOT_LTO（其它平台默认 OFF，未验证）。读数见 docs/03-emulator/aot-lto-evaluation.md §10。
- tags: [emulator-lift, lto, efficiency]

## 设问

`lift-gen` 发射 target 函数属性（253cbe0）之后，LTO 到底能不能把 `lift_tick` / `lift_note_write` 内联进
AOT？以 FELLOW 进入段那个高计算密度窗口为样本，「指令总量 + 耗时」的收益是否可见到足以改默认？

## 结论（2026-11-17 / mac arm64 / Apple clang 17 / RelWithDebInfo）

**能，且可见 ⇒ mac 默认开启**（用户裁决：只要有可见优化就接受默认开）。

### 静态（A = OFF / B = ON `-abi`；同机同天）

| 量 | A | B | Δ |
|---|---:|---:|---:|
| `__text` | 14,399,532 B | 14,852,364 B | +3.14% |
| 总 native 条数 | 3,608,111 | 3,721,295 | +3.14% |
| lift 条数 | 2,500,533 | 2,689,677 | +7.56% |
| `_lift_tick` 站点 | 101,765 | 13,893 | **−86.4%** |
| `_lift_note_write` 站点 | 38,535 | 0 | **−100%** |
| `_lift_dyn` 站点 | 5,658 | 5,678 | +20 |

### 窗口（FELLOW init；10 run 交错 A,B×5；每 run 全新 appdata）

- `wall` 中位 **621.27 → 542.96 ms（−12.61%）**，逐对 5/5 全负（−11.88…−13.17%）
- guest `insns` 中位 492.36M vs 492.53M（+0.03%，抖动内，⛔ 不作效率判据）
- 桶中位：lift −19.5%、boundary −11.0%、shim 不变 ⇒ 省下的全在 lifted 代码段
- 语义指纹 10/10 逐位相同：`exit 0` · `frames 6000` · `stop @ 00498AE4` ·
  arena `125625400/125625400/31` · `table misses 0`

### 代价

AOT 链接 **0.16 s / 250 MB → 37.42 s / 3.03 GB**；二进制 +6.15%；每次改 IR / 运行库都付这笔。

## 落地

`apps/emulator-lift/CMakeLists.txt`：`LIFT_AOT_LTO` **APPLE 默认 ON**、其它平台默认 OFF；
`LIFT_AOT_LTO_ABI` ON、`_RUNTIME` OFF；形状 `split8-O2-lto-abi`；回退 `-DLIFT_AOT_LTO=OFF`。
口径见 `AGENTS.md` §12.8 第 3 条；读数与实际配方见 `docs/03-emulator/aot-lto-evaluation.md` §10。

## 未做

- win32 / Linux 的 LTO 未验证（默认 OFF）⇒ 已派生单。
- 只测了 `fellow-init` 一个窗口（0 帧、纯计算段）；音频 / 真 GPU 呈现 / 存档写盘未覆盖。
