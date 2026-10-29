# spec 已成篇但参考实现仍是桩（0x2c7 / 0x2de / 0x341·0x345·0x34e·0x352 / 0x2ee / 0x25b）

- id: REQ-01M63YS3T54VYCXCBJ6RPCHDAY
- type: req
- status: open
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- order: 9
- tags: [emulator-cxx, spec divergence]

## 是什么

一批 **「spec 已成篇、参考实现仍是桩 / 归属错」** 的分歧（按 `amayui-spec` §9.2 归因 = **实现偏**，不是 spec 缺）。
它们不影响 spec 门禁，但会让"实现了"与"按合同实现"混为一谈 —— 而且这些 opcode **已经派发**（不会停），
所以"脚本足迹 ⊆ 已实现集合"这条判据在这里是**漏的**：它们静默地不做事，或者做错事。

## 清单（2026-10-29）

| opcode | spec 条目 | 参考实现现状 | 差在哪 |
|---|---|---|---|
| `0x2c7` | `CORE-OPS-TEXT-SUBSTR-2C7` | `op_2c7` 空返回 | 不读操作数、不写落点、不做首 / 尾修正 |
| `0x2de` | `CORE-OPS-TEXT-FONT-NAME-2DE` | `op_2de` 恒写 −1 | 没有字体名表 ⇒ 任何名字都查不到 |
| `0x341` / `0x345` / `0x34e` / `0x352` | `CORE-OPS-L2D-LOAD` / `CORE-OPS-L2D-MARK` | 四者共用 `op_341`，空返回 | 不读操作数、不发载入请求、失败不抛 |
| `0x2ee` | `CORE-CONFIG-MESSAGE-FADE-2EE` | 只写一个标量格 | 没写命名键 `message:MessageFade` |
| `0x25b` | `CORE-OPS-RENDER-DISPLAY-STATE-25B` | 只写两个标量格 | 没有"窗口态才载图"那一步 |
| `0xb7` | `CORE-OPS-AUDIO-FADE-TARGET-B7` | 当 BGM 播放实现（`bgm_track_` / 停请求 / 第 9 位） | 引擎改的是**音乐音量渐变对象**的字段（目标 / 开启 / 两端值 / 进度），不是曲目号 |
| `0xb8` | `CORE-OPS-AUDIO-FADE-STOP-B8` | 当 BGM 停止实现 | 引擎清的是渐变对象的"开启"与另一格，不是"停止当前曲目" |

★ `0xb6` / `0xba` 是**按条目实现**的（槽界、静默门、两格清都有），不在本单。

## 判据（每条二选一，都要会红的守卫）

1. **按条目实现**，并在 `core_selftest`（或具名 CTest）里断言 —— 断言要能区分"空返回 / 归属错"与"按合同实现"
   （例如 0x2c7 断言落点变成子串、0xb7 断言渐变对象的**目标值**变而 `bgm_track_` 不变）。
2. 或登记成"本版不实现"（`repo:apps/emulator-cxx/SCOPE.md` + 条目残差），把实现里那句"未建模"改成指向本单。

## 先做哪条

`0x2c7`（`SBSubstr`）与 `0x2ee`（双写）最便宜且最常被脚本用；`0xb7`/`0xb8` 是**归属错**（不是缺），
改起来要先在实现里把"渐变对象"建成一格状态（目标 / 开启 / 两端值 / 进度），再撤掉 BGM 假设；
`0x2de` 需要先有字体名表的填充点（另见 `RES-OPS-2DE-TABLE-SOURCE`）；L2D 四条要接一个子系统，放最后。
