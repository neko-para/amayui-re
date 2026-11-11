# 将 ftol 的 override 加入 override（__ftol2_sse → 函数级 native override，并入 all）

- id: REQ-01M75PW4MXS06MN6FKGXB36ETH
- type: req
- status: open
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- tags: [emulator-lift audio performance native-override]

# 将 ftol 的 override 加入 override（`__ftol2_sse` → 函数级 native override，并入 `all`）

## 目标

把 CRT 的 float→int 辅助 **`__ftol2_sse`** 做成函数级 native override，并注册进 `--native-overrides all`。
它是 `sub_48D610`（Ogg 解码）底下**每句都能看到**的一段纯数值核，语义面比 Huffman/码本小得多。

## 证据（本机 2026-11-12，`graphs/sn0000.graph` 的 `click1` / `click2` 两片窗口，`--native-overrides all`）

| 边 | click1 | click2 |
|---|---|---|
| `sub_48D610 -> __ftol2_sse` | **176.4 ms / 66 176 次** | **172.5 ms / 66 496 次** |
| 同窗口的整条 VOICE（`sub_420F70 -> sub_4BB840`） | 1728.4 ms | 1807.9 ms |
| 同窗口的预填链（`sub_4B73E0 -> sub_4B6C30`） | 1241.2 ms | 1219.4 ms |

⇒ 单句占比 ~10%（VOICE）/ ~14%（预填链），而且它同时在 **feeder 每一刀 fill** 的热路径上
（`sub_4B6C30 -> sub_48D610`）。

## 先取证（⛔ 不要照"截断"直觉写）

1. **入口 EA 与归类**：`.lst` 里 `__ftol2_sse` 的标签是 `0x4E82F6`，但 `disasm-fn show 0x4E82F6`
   报"不落在任何 `proc near` 里"（`0x4E82F6` 不在 item 目录）⇒ 它是**被折叠的库函数**那一族，
   锚按 `.lst` 的 EA 记，准入与判据都别依赖 item 目录。
2. **调用约定与返回**：st0 还是 xmm0 进、edx:eax 还是 eax 出（`__ftol2` 家族通常返回 64-bit 在 edx:eax）。
3. **边界行为**（最容易静默错的地方）：0 / −0 / 超出 int64 / NaN / ±Inf 的返回值；是否看
   x87 CW 或 `MXCSR` 的舍入模式（`_ftol2` 的语义是**向零截断**，与"当前舍入模式"不同）。
4. **它是否读/写任何 guest 内存或全局**（若只碰寄存器，准入可以极窄）。

## 做法（照 `docs/03-emulator/design-notes.md` 的 `L3-mdct-01` 纪律）

- `src/runtime/native_ftol.{h,cpp}`：纯核 + guest 入口只做准入（**所有回退原因在任何写之前判掉**）。
- 注册到 `NativeOverrideTable`，进 `all`；保留 `all,-__ftol2_sse` 做隔离臂。
- **oracle = 解释器臂**（真 guest 字节），⛔ 不手写参考实现（那会让"两边都错"看不见）。
- 判据要含**边界个案**：0 / −0 / ±1 / 2^31−1 / 2^31 / 2^62 / 2^63 / 越界 / NaN / ±Inf，
  以及 in-place 不适用（纯寄存器函数）但 **x87 栈顶 / 状态字** 必须两臂相同。

## 判据（红了意味着什么）

- `apps/emulator-lift/tests/native_override_assets.cpp` 的 `ftol/*`：同一输入、**解释器臂 vs native 臂**
  逐值比（含上面的边界表）；并要求 `hits>0 && fallbacks==0`（排除"没接管 / 每次回退"两种假绿）。
- `apps/emulator-lift/tests/native_override.cpp`：准入零写（例如人为把一条前提弄坏 ⇒ 输出与寄存器一字节不变）、
  成功路径 ABI（EIP / ESP / EAX / EDX / x87 top / status）。
- `tools/mutate-check.mjs` 加一条代表（例如把"向零截断"改成"四舍五入"⇒ 上面的差分当场红）。

## 收口读数

同一 `graphs/sn0000.graph` 窗口：`sub_48D610 -> __ftol2_sse` 应不再出现在函数表里（被 override 替代），
`sub_420F70 -> sub_4BB840` 与"一刀 fill"的墙钟同时下降。
⛔ 不断言具体数值（`AGENTS.md` §10：数字是读数，不是断言）。

## 未做

- 66k 次/句的**调用点族**归因（谁在调、能不能顺带把调用侧的整段循环 native 化）——先做本单，
  归因结果再决定要不要另开。
- 与 `sub_4E4370 -> sub_4E3EE0`（Huffman/码本，~600 ms/句）的关系：本单**不**处理那一段，
  见 `REQ-01M75PTEGCR1Y91FKKJ1CWPNH2`。
