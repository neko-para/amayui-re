# 🐞S2 spec 修订后实现与守卫未对齐（0x320 地址读 / 窗收尾 / 0x9 复位 / 0x6 argc）

- id: REQ-01M5ED27BF1T6NSEJ05GT2XFW3
- type: bug
- status: done
- parent: REQ-01M3TCB0S0BTW41KW0JBJMWHHH
- verify: tools/test/emulator-new-scene.test.mjs#★ 窗收尾：绘制项与网格都清"有窗"位
- repro: tools/test/emulator-new-scene.test.mjs#★ 窗收尾：绘制项与网格都清"有窗"位
- severity: S2
- tags: [emulator, spec-drift]

## 问题（本轮已全部处置）

spec 按 `.lst` 改了几处与参考实现相反的合同；`apps/emulator-new` 与守卫已按新正文对齐：

| 项 | spec（真源） | 处置 | 守卫 |
|---|---|---|---|
| `0x320` | 第 2..8 个操作数**全按地址读**；前两段逐字原样拷贝；第三段 =（解码后低 8 位 << 24）\|（解码后低 24 位）；第 4/7/8 个地址交给顶点缓冲对象（外部） | 重写 `scene320`；`Mesh` 改成 `copies[2] + colors + vbo`（删除旧的 `weights/scale/uv` 误读） | `emulator-new-scene.test.mjs#★ 0x320：2..8 全按地址读 ⇒ 前两段原样拷贝、第三段合成、4/7/8 交给顶点缓冲对象` |
| 窗收尾 | 到期时**绘制项与网格都清**"有窗"位；持续时间 ≤ 0 不推进 | `stepScene` 两处都清位；注释与测试标题同步 | `emulator-new-scene.test.mjs#★ 窗收尾：绘制项与网格都清"有窗"位`（原 repro 现在通过） |
| `0x9` | 丢 40 帧槽、帧号 0、父帧号 −1、**状态选择字归 0**、三个全局池回初值 | 补 `flags = 0` 与三个全局池清空；帧侧仍受 LIFO 模型限制（见下） | `emulator-new-frame.test.mjs#★ 0x9 全局重置：丢全部帧、清空三个全局池、状态选择字归 0，并以 id 0 重装根脚本` |
| `CORE-TIMING-WINDOW` 证据 | 旧守卫断言"绘制项不清"，与 `.lst` 相反 | 守卫改成"都清"后重新挂回 evidence，规则回到 `reviewed`，删除 `RES-VM-WINDOW-GUARD-STALE` | 同上窗收尾用例 |

## 未做（有明确归属，不算本单欠账）

- **父帧号 −1**：参考实现的帧模型是 LIFO 栈、没有"帧号/父帧号"字段（引擎是 40 槽 + `cur`）⇒
  这条只能在帧模型重构后落地，已由 `REQ-01M5D251ZJTZA4S6SX170RAXD2`（S2）跟踪；本单不重复建模。
- **`0x6` / `0x8`**：spec 明确写"本版不规定"（`RES-FRAME-LOAD-FRAME` / `RES-FRAME-CALL-FRAME`，都带 next）⇒
  参考实现**保持响亮失败**（未实现即抛），不按猜测实现。★ 原先记的“`0x6` 的 argc=2 与长度槽 3 不一致”是误判
  （0x6 的 handler 写 5 = 2×2+1；写 3 的是 0x8）—— 已由 `REQ-01M5ED2AD7SKV077BRVRGNE3R7` 的对账守卫覆盖（446/544 全一致）。

## 判据（已满足）

- `pnpm test` 594/594（pure）· `pnpm test:assets` **126 pass / 2 fail（均为先前已知的 mac 路径语义项）/ 26 skip** · `pnpm typecheck` 四工程绿
- `pnpm tools spec validate` 13/13
