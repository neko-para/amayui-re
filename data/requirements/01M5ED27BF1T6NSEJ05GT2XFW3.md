# 🐞S2 spec 修订后实现与守卫未对齐（0x320 地址读 / 窗收尾 / 0x9 复位 / 0x6 argc）

- id: REQ-01M5ED27BF1T6NSEJ05GT2XFW3
- type: bug
- status: open
- parent: REQ-01M3TCB0S0BTW41KW0JBJMWHHH
- repro: tools/test/emulator-new-scene.test.mjs#★ 窗收尾不对称：网格清"有窗"位、绘制项不清
- severity: S2
- tags: [emulator, spec-drift]

## 问题

本轮 spec 按 `.lst` 改了几处**与参考实现相反**的合同，`apps/emulator-new` 与守卫仍按旧语义：

| spec 现在 | 实现 / 守卫现状 | 判据 |
|---|---|---|
| `0x320` 的第 2..8 个操作数**全按取址读**；第三段逐元素"解码后低 8 位进高字节 / 低 24 位进低 24 位" | `handlers.ts` 的 scene320 把 7/8 当 `readFloat`、第三段未解码 | `.lst` 0x4321ac / 0x4321c3 / 0x432287 |
| 窗到期**绘制项与网格都清**"有窗"位；持续时间为 0 不推进 | `machine.ts` 按"绘制项不清"实现；`emulator-new-scene.test.mjs#★ 窗收尾不对称` 断言旧语义 | `.lst` 0x49BC99→0x49BCA6 / 0x4AF3C9 |
| `0x9` 复位清单（40 帧槽 / 帧号 0 / 父帧号 −1 / 三全局池 / 状态选择字） | 旧实现可能缺父帧号 −1 | `.lst` 0x40DF44 / 0x40DF69 / 0x428A80 |
| `0x6` 的 `argc=2` 与长度槽 3 不一致（装载帧） | 未实现 `0x6`/`0x8` | `.lst` 0x41c854 / 0x41c940 |

## 判据

- 实现与 spec 一致后，把 `CORE-TIMING-WINDOW` 的 evidence 重新挂回"都清"的新守卫（现为 draft，残差 `RES-VM-WINDOW-GUARD-STALE`）。
- 每条都补一条**会红**的守卫用例（改坏语义 ⇒ 红）。

## 不做

- 不为了迁就实现改 spec（spec 是真源，按证据裁决）。
