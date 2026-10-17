# 批 S2 · 全语料函数建模（2579 个有形状内容的函数）：传播优先分批 + 每批机器验收（载体单文件 carriers.json）

- id: REQ-01M54KEF7X560CMPMGABG2W3CW
- type: req
- status: open
- parent: REQ-01M52F3ASMGWQXX64JSA18MP2N
- order: 52
- tags: [engine shape]

## 目标

把 **2579 个有形状内容的函数**逐个建模，**按类型传播顺序**推进，每批用机械判据验收。
口径不是"全部 L0 闭合"，而是**全部完成分类与声明，能闭合的闭合，闭不了的按原因归档**。

## 口径（已定）

| 项 | 决定 |
|---|---|
| 载体落点 | **单一** `data/shape/carriers.json`（不分片、不记历史、直接更新）；话题是查询不是存储轴 |
| 类型落点 | `data/shape/types.json`（中心表）；**只有主代理写** |
| 谁来分析 | **subagent**；产物写 `.tmp/shape/batch-NN/<批号>/`（proposal + report + logs） |
| 谁来合并验收 | 主代理：逐条核 witness → 合并 → 六条判据 |
| 外部库 | 不单独标注；无 obj0 访问就在 `notes` 写"疑似外部库 + EA"，不硬声明 |

## 批次顺序 = `pnpm tools shape propagate`（机械依据）

从载体出发沿调用图 BFS：`this`/`interior` ⇒ 同类；`member(path)` ⇒ 解析目标类；`global(sym)` ⇒ 从根（`kind:"global"` 载体）正向走；`unknown` ⇒ 不传播。
⛔ **不许按"偏移命中率"猜类**（低命中既可能是别的类，也可能只是成员表没覆盖）。
机制细节与被否掉的做法见 `docs/02-engine/shape-census.md` §7.7。

## 每批验收（六条，缺一不过）

`claims types.json` / `claims carriers.json` exit 0 · `calls` 无 `mismatch` · 变异全红预期 ·
`pnpm test` 全绿 · `requirements validate` 5/5 · **闭合度不下降**（下降必须写原因）。
★ `coverage` 的"未解析基址"要**分两档读**（保守/校准），差额不是缺口。

## 反模式

⛔ 为"看起来完整"编偏移 · ⛔ 拿"偏移相同"顶替类型 · ⛔ 批量照抄提案（必须逐条核 witness）·
⛔ 让 subagent 直接写 `data/shape/**` · ⛔ 把"通用函数"（如 `sub_408050` 那种 printf 类）声明成某类的载体。
