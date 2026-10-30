# spec 已成篇但 cxx 仍是桩（0x2c7 / 0x2de / 0x341·0x345·0x34e·0x352 / 0x2ee / 0x25b）

- id: REQ-01M63YS3T54VYCXCBJ6RPCHDAY
- type: req
- status: open
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- order: 9
- tags: [emulator-cxx, spec divergence]

## 是什么（2026-10-29 登记；2026-10-31 复核仍成立）

一批 **「spec 已成篇、`apps/emulator-cxx` 的实现仍是桩 / 归属错」** 的分歧
（按 `amayui-spec` §9.2 归因 = **实现偏**，不是 spec 缺）。
这些 opcode **已经派发**（不会停），所以"脚本足迹 ⊆ 已实现集合"这条判据在这里是**漏的**：
它们静默地不做事，或者做错事。

★ 本单只说 **cxx**。`apps/emulator-new` 已废弃（冻结）⇒ 那边的同名桩**不再处理**，
⛔ 不要用"参考实现也这样"当理由。

## 清单（2026-10-31 对代码复核）

| opcode | spec 条目 | cxx 现状 | 差在哪 |
|---|---|---|---|
| `0x2c7` | `CORE-OPS-TEXT-SUBSTR-2C7` | `op_2c7` 空返回 | 不读操作数、不写落点、不做首 / 尾修正 |
| `0x2de` | `CORE-OPS-TEXT-FONT-NAME-2DE` | `op_2de` 恒写 −1 | 没有字体名表 ⇒ 任何名字都查不到 |
| `0x341` / `0x345` / `0x34e` / `0x352` | `CORE-OPS-L2D-LOAD` / `CORE-OPS-L2D-MARK` | 四者共用 `op_341`，空返回 | 不读操作数、不发载入请求、失败不抛 |
| `0x2ee` / `0x25b` | 文本槽 / 显示状态 | 只存观测格 | 没有消费者合同 |

## 判据

每条落地 = 读操作数 + 改引擎状态 + **会红的守卫**（CTest 顶层注册，或 `mutate-check` 一条）。
做不到就登记"本版不实现"（`repo:apps/emulator-cxx/SCOPE.md` + 条目残差带 `next`），⛔ 不许留空返回。
