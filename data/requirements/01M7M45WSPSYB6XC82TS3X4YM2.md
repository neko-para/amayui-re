# SC0000 前置：加载确认框（SBUNKI）不接受注入点击 ⇒ SC0000 到不了

- id: REQ-01M7M45WSPSYB6XC82TS3X4YM2
- type: req
- status: blocked
- parent: REQ-01M7G7EJF062NKQ0D40RF3BZ4Z
- blocked_by: [REQ-01M7HFWQ4ADJD20CRC9PXSFQJW]
- tags: [emulator-lift, input, sc0000]

## 现象（2026-11-17 / mac arm64 / HEAD 4272091 / `--game-dir links/raw`）

加载存档屏 → 选槽 001（缩略图台词逐字 = SC0000 第一句）→ 弹 `SBUNKI.BIN`「ロードしますか？」。
**`はい` / `いいえ` 都点不动**：注入点击的 `fired/released` 递增、`SBUNKI` 的鼠标回调也跑了
（`pos` 采到 `0x30A4` = 回调尾 `sub_30A4`），但确认动作块 `[0x30AC,0x32E8)` **从未执行**。

⇒ SC0000 的渲染路径**根本到不了**（这比任何 D3DX 缺口都靠前）。可复现：4 次独立 run
（`.tmp/sc0000/run-raw{8,9,10,15}.log`）同一步停住；截图 `.tmp/sc0000/shots/`。读数见
`.tmp/sc0000/read2.md`。

## 为什么不是"坐标不对"

- 槽 001 的点击**生效了**（缩略图出现、确认框弹出）⇒ 坐标 + `click` 投递本身是通的；
- 确认框的 `はい` 有 hover 高亮（截图）⇒ 光标位置对；
- 6 种投递形状都试过（`[0x9F8,0xB18)` / 主轮询体 `[0x9A4,0x9F8)` / `i2fc` 段 `[0xA14,0xAF8)` /
  `いいえ`(640,396) / 连点两次）⇒ 全部 `fired+1` 但框不动。

## 静态观察（SBUNKI 清单，⛔ 未过 §6 准入门，只是"读到了什么"）

- 主轮询 `0x9A4`：`get-input-type` → `eq 9e, 90, 1` → `jcc 9e 0x30AC ffffffff`；不成立就
  `sleep 1` + `jmp 0x9A4`。即门是 **`get-input-type == 1`**。
- 回调里（`0xB70` 之后）还要求 `(global-int f8fa5) & 4` 非 0 才 `mov 90 1`（= 让门成立）。
- `get-input-type` 的 opcode 语义**未登记**：见 `REQ-01M7HFWQ4ADJD20CRC9PXSFQJW`（0x0CD）。

## 现在的判断

要么是 **`get-input-type` 读的那份状态**没被注入点击填上（→ 本仓 input/shim 面的缺口），
要么是 `f8fa5` 的写者需要另一条输入路径（键盘 / 消息边沿）。两者都要先有
`REQ-01M7HFWQ4ADJD20CRC9PXSFQJW` 的结论才判得动 ⇒ 本单**blocked_by** 那张 spike。

## 判据

- 机制定了之后：把这条转成 `bug`（带 `repro`）或直接实现（若属 shim 缺口），
- 验收 = 同一配方能过确认框、`await script SC0000` 命中；守卫按机制选（input 面优先
  `input-plan-read-points` / `lift-control-endpoint` 那一族）。
