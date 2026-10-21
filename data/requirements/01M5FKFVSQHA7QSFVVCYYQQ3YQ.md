# 第四轮复核剩余取证：0x32f 同一性 / 位置表语义 / 颜色位段 / 负组号 / file_index

- id: REQ-01M5FKFVSQHA7QSFVVCYYQQ3YQ
- type: req
- status: open
- parent: REQ-01M5D1G31PN9EGANRVAR753BPC
- tags: [spec, evidence, review]

## 一句话

第四轮复核（`repo:docs/02-engine/spec-review-4.md`）里**必须再取证才能定**的剩余项。文本清理（S2/S3 30 组）已由 `REQ-01M5FBD5…` 完成；本单只跟踪"还缺哪条观察"。

## 清单（每条 = 一个证据缺口；对应 audit 残差已带 next）

| # | 缺口 | 现状 / 下一步 |
|---|---|---|
| 1 | `0x32f` 的容器与 `0x8003` / `0x8009` 句柄分支是否同一对象 | 两处规则已互引并写明"未取证"；下一步：对同一脚本跑两条路径，看登记表是否同一 |
| 2 | 三张位置表（`0x71` / `0x3` / `0x8f`）的**内容语义**与消费者 | 单位与基已在 `15-frame` 统一；剩余"每张表服务哪些 opcode、存档写入侧怎么产生下标"无观察 |
| 3 | 颜色哨兵 / "不透明黑"的数值与 alpha 位段 | `0x1f9` / `0x202` / `0x203` / `0x322` / `0x323` 已降 `unhandled`，残差 `RES-*-COLOR` 指向"位段取证" |
| 4 | 负组号的处置 | `60-ops` 已写"本版不规定"；若要成合同，需在 handler 里确认拒绝 / 抛异常 / 静默哪一支 |
| 5 | `FMT-ALF-TOC` 的 `file_index` 用途 | `audit` 残差 `RES-ALF-FILE-INDEX`（unknown）；下一步：找读该字段的代码 |

## 判据

* 每条要么给出 EA 观察并改正文（带 `KN:` 或 `guard:`），要么保持"未规定"+残差 `next`；
* ⛔ 不许用"没人读 / 结构上不会走到"当证据（方法单反模式）。

## 怎么查

`pnpm tools spec report` · `pnpm tools spec show <规则 id>` · `pnpm tools spec chapters`。
