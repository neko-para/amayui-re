# M-lift-4 · 欠账：lift_note_write 的 SMC 缺口 + guest 寄存器跨块 SSA（tick 归零已收口）

- id: REQ-01M7FRY04PTP9788QZPVPKA5ND
- type: req
- status: open
- parent: REQ-01M7JHSEZVMYB4XHEN32V7V6HJ
- tags: [emulator-lift, lift, debt]

## 这份欠账是什么

`M-lift-2` 的判据 1–3 里，**判据 1**（`lift_one` 归零）在做；**判据 2、3 按用户口径降级为欠账**，理由是
`OBS-20261115-06` 的测量：翻译侧已 1.7 ns/条，派发只剩 ~0.3 s，剩下的墙钟 78% 在图形 shim。
⇒ 这几项**不再是速度杠杆**（合计 ≤1–2% 墙钟），但它们是"交付物里没有解释器 / 真翻译形态"的字面判据，
所以单独挂账、而不是丢掉。

## 欠的三笔（第 1、2 笔的"归零"已随 LTO 完成；SMC 缺口与 SSA 未做）

1. ✅ **`lift_tick` 归零 —— 2026-11-17 完成（不需要 IR 记账改造）。**
   生成物里 `@lift_tick` 的调用点 **0**：253cbe0 发射 target 函数属性 ⇒ LTO 真内联
   （native `bl` 101,765 → 13,893）⇒ 去判空（13,893 → **0**，`_lift_tick` 符号被删）。
   `__text` 14,852,376 → **13,903,808 B（−6.39%）**，比 LTO OFF 的 14,399,532 B 还小 3.44%。
   读数与 census：`docs/03-emulator/aot-lto-evaluation.md` §10 / §11。
   原来的"难点 / 形状"（SSA 累加 + 函数出口落账）**不再需要**：整个 callee 被内联进每个站点。
2. **`lift_note_write` 归零 ✅（随同一轮 LTO）+ 补上被它掩盖的 SMC 缺口 ⬜**：调用点
   38,535 → **0**（符号被删；内联体改调 `noteGuestWrite`，38,698 站点）。但它在 AOT 臂里仍是
   **no-op**（`cpu.cache == nullptr`）⇒ guest 写代码页时，已发射的 lifted IR 仍执行旧语义。
   - 已裁决的形状（`docs` 内部的 D50）：**可执行节宿主页设只读 + VEH 归类成 SMC 响亮停**。
     代价 ≈0 B/站点、−6.94%，且覆盖**四层**（翻译器 store / push-pop / `lift_one` 内部的 `rep movs`·SSE / shim 裸 `wr32`）。
   - 前提可证：8 个节的 begin/end **全部 4 KiB 对齐** ⇒ 4 KiB 宿主页上"可执行页集合"逐页精确。
     ⚠ **16 KiB 宿主页 = macOS arm64 上不成立** ⇒ 必须留 `--smc=inline` fallback + 启动拒绝。
   - ★ 附带收益：删掉这个函数顺带消掉它的 `argmemonly` 声明 —— 那是一句**有条件成立的谎**（它经 `cpu->cache` 写别处）。
3. **guest 寄存器跨块 SSA**：今天 71,929 个 guest 块边界各刷一次 9 个槽（≈288k 次 `%st` 访存）。
   - ⚠ **反直觉的账**：出口只有 38,950 个，但"出口写回全部 9 个槽" = 350k 次 store ⇒ **净值是负的**。
     ⇒ **may-dirty 分析不是优化，是这一步成立的前提**；带上它才 −0.85~−1.0 MB（体积 −10%~−11%）。
   - 形状：9 个 `alloca` + `mem2reg` 构造 SSA（⛔ 不手写 phi），配结构性守卫"`-O2 -emit-llvm` 后 `lift_cc_*` 里 `alloca` 数必须为 0"。
   - ★ 硬依赖：必须在 1、2 之后（块边界/每个 store 上的 `call` 会 clobber caller-saved，保活值全要溢出）。

## 判据

- ✅ 生成物里 `@lift_tick` 的调用点 **= 0**、`@lift_note_write` 的调用点 **= 0**（2026-11-17 读数：
  `llvm-objdump -d | rg -c 'bl\s+.*_lift_tick'` = 0，`llvm-nm | rg _lift_tick` 为空；§11）。
  ⚠ 这一条**只能当读数**：它依赖 LTO 形状（`-DLIFT_AOT_LTO=OFF` 时调用点必然回来），
  没有可移植的 CTest 锚；⛔ 不要为它写一条依赖 shape 的测试。
- ⬜ SMC 缺口（第 2 笔的后半）未做；寄存器 SSA（第 3 笔）未做。
- 寄存器：`objdump -d` 里热函数体内不再出现那组 9 槽收尾的 `mov [rXX+off], reg`；
- 语义不变量：`exit` / `frames` / `stop @` / arena / `misses` 逐位相同；`ctest` 全绿；
- 每一笔都要有**能红**的守卫 + 变异条目（⛔ 不要断言当前数字）。

## 边界

⛔ 不要用"放宽语义"换这几笔（例如把 `insns` 变成近似值）。⛔ 不要顺手改排 `AGENTS.md` 的章节号。

## 静态基线（2026-11-17 · arm64 · RelWithDebInfo · commit `bae9299`）

只读普查：`.tmp/insn-overhead/report.md`（603 行；`sampler.mjs --seed 20261117` 可重跑）。

- 10,254 个 `lift_cc_*` = **2,500,533** native 条 / 10,002,132 B；**host/guest = 7.18**（边界税 1.99 + body 5.19），每 guest 块 **34.73** 条，平均块长 **4.84**。
- Σ`lift_tick` 参数 = **348,208**（闭包静态 guest 指令）；tick 调用 **104,949**(IR) / **101,765**(native) = 1.46 次/块、0.30 次/guest 指令（**不是每条 guest 指令一次**）。
- `@lift_note_write` 站点 **38,535**；最长块 `bb_435800`（`4765B0`）一个块 **297 次 note + 298 次 tick = 595 次宿主调用**。
- 静态块边界税 **693,001 = 27.71%**；没有单桶 ≥10% 可一刀砍（最大 `movImm` 14.0%）。tick 站点级 12.2%，callee 另有 5 条 ⇒ 一次 tick 执行 8 条；可改性在**内联**（`ldr/add/str`），预计动态 −50%~−62% 的 tick <待核>。
- 第 1 笔（tick 归零/内联）的静态依据；第 2 笔（note 归零）的 297 连发是音频卡顿的头号静态线索（<待核>：`noteGuestWrite` 实际成本）。
- 口径：arm64 静态数，⛔ 不是 win x64 墙钟；`27.71%` 不能拿去复核 profile 的 `boundary 29%`（不是同一个量，见报告 §5）。

## tick 判空（2026-11-17 复核 → 已落地）

- `lift_tick` 的唯一调用方 = 生成物；runtime/tests 零调用，没有任何测试断言 null；`cpu` 恒为 `&AotRunner::cpu_`。
- **已按此口径落地（14d331a）**：`if (!cpu_ptr)` 删除、改 `assert(cpu_ptr != nullptr)`；
  `lift_one` / `lift_shim` 的判空保留。读数见 `docs/03-emulator/aot-lto-evaluation.md` §11。
