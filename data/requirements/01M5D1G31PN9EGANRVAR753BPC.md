# spec 组织：ops 分片（按族拆章）+ 跨机制规则的归属普查（storage 跨族必须显式 refs）

- id: REQ-01M5D1G31PN9EGANRVAR753BPC
- type: req
- status: open
- parent: REQ-01M5851Z51V1MTSWH414M7WN5Q
- tags: [engine spec, organization]

## 一句话

`CORE-OPS` 是**目录层**（必须覆盖全部 opcode）。本单把两件事变成可执行：**按族分片**（避免单文件无限长）与**跨机制归属口径**（"40 个帧槽"归谁）。

## A. ops 分片（2026-10-22 完成）

原单文件 `60-ops.md`（已删除）已拆成 **11 个族文件**（比原方案多 `ops-array` / `ops-text` 两族：分片方案写在数组族与启动前段批次之前）：

| 章 id | 文件 | 规则 / opcode |
|---|---|---|
| `ops-numeric` | `repo:data/spec/chapters/60-ops-numeric.md` | 8 / 20（数值算术·比较、位操作、搬运） |
| `ops-array` | `repo:data/spec/chapters/60-ops-array.md` | 5 / 5 |
| `ops-text` | `repo:data/spec/chapters/60-ops-text.md` | 1 / 1 |
| `ops-frame` | `repo:data/spec/chapters/60-ops-frame.md` | 8 / 8 |
| `ops-texture` | `repo:data/spec/chapters/60-ops-texture.md` | 5 / 5 |
| `ops-scene` | `repo:data/spec/chapters/60-ops-scene.md` | 5 / 5 |
| `ops-voice` | `repo:data/spec/chapters/60-ops-voice.md` | 2 / 2 |
| `ops-list` | `repo:data/spec/chapters/60-ops-list.md` | 8 / 12 |
| `ops-forward` | `repo:data/spec/chapters/60-ops-forward.md` | 4 / 8 |
| `ops-scalar` | `repo:data/spec/chapters/60-ops-scalar.md` | 9 / 18 |
| `ops-wait` | `repo:data/spec/chapters/60-ops-wait.md` | 5 / 5 |

合计 **60 条规则 / 89 个 opcode**（与拆前逐条对账）。规则 id 跟族改了前缀（`STRING-SET→TEXT-SET`、`SUBSYSTEM-*→LIST-SUBSYSTEM-*`、`POLL-INPUT→WAIT-POLL-INPUT`），bindings/audit/引用同批同步；分片**没有重写正文**。

## B. 跨机制规则：先不造工具（★ 2026-10-21 决策，保留）

跨机制发现暂不做专门工具与 `refs` 字段：这类交叉现在只有"帧栈容量 / 当前帧"一处，且覆盖率（`covers` + 试点闭合）会先把"解释不圆"暴露出来；真要做应由 coverage 缺口驱动，⛔ 不预先造矩阵工具。

## C. "40 个帧槽"归 frame（已落地）

归属判据是**谁拥有不变量与生命周期**：容量上限、父帧链、当前帧选择、按帧独立（池 / 返回栈 / 指令指针）、超限即错都在 frame。
规则 `CORE-FRAME-STACK` 挂在 frame 章，`storage` 指向 `shape:T_Command@0x5d894`（帧区 `T1[40]`）与 `shape:T_Command@0x5d880`（`cur`）——★ 旧写法 `FRAME_LAYOUT.*` / `layout:` 已被 `resolveStorage` 结构性拒绝，不要再按它挂。

## 剩余（本单不收口的原因）

* `REQ-01M5FBD4…`：未成篇 opcode 的 `gaps[]` 落点与 report 计数（分片后按章派生，仍由那张单做）。
* `REQ-01M5FKFVS…`：第四轮复核剩余取证（0x32f 同一性 / 位置表语义 / 颜色位段 / 负组号 / file_index）。

## 不做

* ⛔ 分片时不重写正文（只搬移 + 改 id 前缀），避免把"整理"混成"改语义"。
* ⛔ 不为通过校验而放宽 `#12`（章节 ↔ 规则 ↔ 绑定 一一对应）。
