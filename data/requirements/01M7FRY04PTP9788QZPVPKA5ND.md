# M-lift-4 · 欠账：lift_note_write 的 SMC 缺口 + guest 寄存器跨块 SSA（tick 归零已收口）

- id: REQ-01M7FRY04PTP9788QZPVPKA5ND
- type: req
- status: dropped
- parent: REQ-01M7JHSEZVMYB4XHEN32V7V6HJ
- dropped_reason: 两笔均已裁决：SSA 实测判负（aot-alloca-evaluation.md：Cut A __text −0.84%、spill +151%、墙钟 <5%；Cut B 未测估 <2%）；SMC 不修——静态普查 395,092 条 .text 指令 0 条写可执行页，poly 是 289 条只读 IAT thunk，无 PAGE_EXECUTE 分配；残余动态写者未观察到，loud 档代价 1.5–2 天。重开条件：出现 guest 自改代码导致的执行不一致 / 发现真实写点 / 要跑自解压目标。
- tags: [emulator-lift, lift, debt]

## 结论（2026-11-24：本欠账单关闭，dropped）

两笔都有结论，不再保留为"待办"：

1. **跨块 SSA：drop**（实测判负，见 `docs/03-emulator/aot-alloca-evaluation.md`）。
2. **SMC 缺口：不修（裁决）**。静态普查 395,092 条 `.text` 指令里 **0 条**写可执行页；`poly` 是
   289 条 IAT thunk（可达、只读）；无 `PAGE_EXECUTE` 分配、无绝对写 IAT。剩下的动态通道
   （`rep movs` / memcpy 内建 / shim 写）静态不可判，但**没有观察到任何写点**，而 loud 档的代价是
   1.5–2 天 + 页保护 / VEH 的回归面 ⇒ 裁决不修。风险与重开条件见下。

## 为什么可以关（⛔ 不等于"已修"）

- SMC 语义缺口仍然存在：若 guest 写代码页，AOT 臂会静默跑旧 IR。今天**没有观察到这种写者**；
  静态证据在 `REQ-01M86SC1MJFQBHT9MNAADCF8MT`（已完成）。
- 修它需要：可执行节页保护 + VEH 分类 + `--smc=off` + 测试 / 变异；且 `poly` 是第二个可执行节、
  `VirtualProtect` 是 no-op、mac 16 KiB 页不成立。收益是"一个未观察到的风险面"，不是任何读数。
- **重开条件**：① 出现 guest 自改代码导致的执行不一致（开缺陷单）；② 静态 / 动态发现真正的写点；
  ③ 将来要跑一个已知自解压 / 自改的目标。重开时先按 `aot-alloca-evaluation.md` 的方法重做设计，
  ⛔ 不要照抄已消失的 D50；测量用"页保护 + VEH report"（⛔ 不要用 `LIFT_WATCH` / `LIFT_AOT_NOTE_WRITE=ON`
  当"写代码页"的证据）。

## 三笔的历史读数（保留）

1. `lift_tick`：IR **74,691** / native **0**（LTO 内联；`aot-lto-evaluation.md` §10/§11）。
2. `lift_note_write`：默认档 `LIFT_AOT_NOTE_WRITE=OFF` ⇒ 0 个 call / declare；ON 档 38,535→0 是历史读数。
3. 跨块 SSA：Cut A `__text` **−0.84%**、spill **+151%**、墙钟 <5% 分辨力；Cut B 未测、估 <2%。

## 边界

⛔ 不把本单当成"tick/note 的 IR=0"（口径已定 native，见 M-lift-2）。⛔ 不为未观察到的写者加每 store 页检查。
