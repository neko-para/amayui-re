# spec 组织：ops 分片（按族拆章）+ 跨机制规则的归属普查（storage 跨族必须显式 refs）

- id: REQ-01M5D1G31PN9EGANRVAR753BPC
- type: req
- status: open
- parent: REQ-01M5851Z51V1MTSWH414M7WN5Q
- tags: [engine spec, organization]

## 一句话

`CORE-OPS` 章现在是**目录层**（必须覆盖全部 opcode），但它把所有族塞在一个文件里；同时"跨机制规则"（例：引擎持有 40 个帧槽）**没有机械的发现机制**，只能靠人想起来立章。本单把两件事变成可执行：**分片** + **归属普查**。

## A. ops 分片（预先拆分，避免单文件无限长）

现状：`data/spec/chapters/60-ops.md` 约 50 条规则、460+ 行，族之间已用 `##` 分隔（数值 / 纹理 / 场景 / 语音 / 引擎级标量 / 列表项 / 转发型 / 帧类 / 等待与输入）。

**分片方案（按族一个文件，`spec.json` 顺序即阅读顺序）**：

| 新章 id | 文件 | 内容 | 规则 id 前缀 |
|---|---|---|---|
| `ops-numeric` | `60-ops-numeric.md` | 数值搬运（`0x50` `0x51` `0x2d5` `0x55`） | `CORE-OPS-NUMERIC-*` |
| `ops-frame` | `60-ops-frame.md` | 帧类指令（`0x8f` `0x5` `0xa0` `0x3` `0x2` `0x9`） | `CORE-OPS-FRAME-*` |
| `ops-texture` | `60-ops-texture.md` | 纹理族（`0x1f7`–`0x1fb`） | `CORE-OPS-TEXTURE-*` |
| `ops-scene` | `60-ops-scene.md` | 场景 / 颜色（`0x202` `0x203` `0x320` `0x322` `0x323`） | `CORE-OPS-SCENE-*` |
| `ops-voice` | `60-ops-voice.md` | 语音（`0x2f6` `0x2f8`） | `CORE-OPS-VOICE-*` |
| `ops-scalar` | `60-ops-scalar.md` | 引擎级标量（`0x149` `0x21b` `0x252` `0x88`） | `CORE-OPS-SCALAR-*` |
| `ops-list` | `60-ops-list.md` | 列表项（`0x70` `0x71` `0x79` `0x1c1` `0x212` `0x213` `0x25d` `0x303`） | `CORE-OPS-LIST-*` |
| `ops-forward` | `60-ops-forward.md` | 转发型（`0x1ca` `0x324` `0x32f` `0x73` `0x78` `0x2db`） | `CORE-OPS-FORWARD-*` |
| `ops-wait` | `60-ops-wait.md` | 等待 / 输入（`0x21c` `0x101`）+ 外部请求 / 空操作 | `CORE-OPS-WAIT-*` |

**判据**：分片**只搬文本与规则 id 前缀**，不改语义；每片必须有 `##` 章标题、至少一条规则（`bootstrap` 档）；`audit.chapters` 同批增同名项（`status: bootstrap`）；`spec.json.chapters` 顺序 = 上表顺序；`pnpm tools spec validate` 全绿。★ 规则 id 改名**不需要**台账更正（台账锚只有 EA 与守卫用例名，不含规则 id）。

## B. 跨机制规则：先不造工具（★ 2026-10-21 决策）

跨机制 / 交叉规则的发现**暂不做**专门工具与 `refs` 字段。理由（用户口径 + 本仓既有机制）：

- 这类交叉在原理上不多（现在真正跨的只有"帧栈容量 / 当前帧"一处）；
- **覆盖率**已经能暴露它：`pnpm tools spec report` 的试点闭合（effect 归属 / 未登记 = 0）与 `audit` 的 `covers` 会逼着人说明"这条规则解释了哪些 effect"——一条规则若跨了机制，它的 effect 归属会先说不圆；
- 真要做，也应当由 coverage 的缺口**驱动**（先出现归属不清的 effect，再补结构），⛔ 不预先造矩阵工具。

⇒ 本单只保留 A（ops 分片）、C（归属口径）与"不做"清单。

## C. "40 个帧槽"到底属于谁（本单的口径）

**归属 frame**，理由不是"内存挂在哪"，而是**谁拥有不变量与生命周期**：

- frame 拥有：容量上限、父帧链、当前帧选择、按帧独立（池 / 返回栈 / 指令指针）、超限即错；
- engine-state 只拥有："这片存储存在"以及 `cur` 这个整数字段的位宽/位置（它只是 frame 的一个字段，与 `0x5D884 callerChain` 同类）；
- ⇒ 规则 `CORE-FRAME-STACK` 挂在 frame 章，`storage` 指向 `FRAME_LAYOUT.count` / `FRAME_ENGINE_SCALARS.cur`；**不**再在 engine-state 章重复（那边只在"引擎级标量"族里按需引用）。

## 不做

- ⛔ 分片时不重写正文（只搬移 + 改 id 前缀），避免把"整理"混成"改语义"。
- ⛔ 不为通过校验而放宽 `#12`（章节 ↔ 规则 ↔ 绑定 一一对应）。
