# 批 M3 · 台账层（append-only 文本真源 + 派生查询层 + 可删可重建守卫）

- id: REQ-01M3TCQTY0X8T8B0FEY3JPJ761
- type: req
- status: open
- parent: REQ-01M3TC9660ZG1YMB7KAKE4W94B
- order: 50

## 范围
`packages/ledger` + `data/ledger/`：一行一条记录、自带 ULID、按 key 前缀分片；派生 SQLite 落 `.cache/`（gitignore）。

## 判据
（待写：删掉本地 DB 后一条命令能确定性重建（同输入同字节）。）
