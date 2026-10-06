# 批 K1 · 知识清理（只读旧仓）+ 产出候选分级清单供裁决

- id: REQ-01M3TD2TG0077TETH5GDRQSXMG
- type: req
- status: open
- parent: REQ-01M3TC9660ZG1YMB7KAKE4W94B
- order: 110

## 前置（人裁决，不是别的节点）
知识线是否启动由用户口径决定（当前未启动）。
★ 但**清理的判据来自 M3**（`REQ-01M3TCQTY0X8T8B0FEY3JPJ761` 的 schema）：归一化（统一 6 种字段记法 /
剔出 14 条字符串化 JSON）**不许自行发明口径**，否则与台账 schema 回头打架 —— 这正是本题"难以单独启动"的成因。

## 范围
只读旧仓：去沿革、修非法枚举、修主键冲突、统一字段记法、剔出字符串化 JSON ⇒ 产出候选清单。

（实测起点，供执行时对账：`fields.json` 3 条非法 `ANALYZED` / 2 组同 `scope+offset` 双 `confirmed` /
9 组同名跨 scope / `functions.json` 14 条 `fields_used` 是字符串化 JSON / 7 份 JSON 里只有 5 份有
`_doc`+`statusEnum`+`counts`，`fields.json` 与 `functions.json` 是裸数组。）

## 判据
（待写实。要点：产出**候选分级清单**供裁决；清单本身就是产物，但**不得**先于 K3 落进 `data/ledger/`。）
