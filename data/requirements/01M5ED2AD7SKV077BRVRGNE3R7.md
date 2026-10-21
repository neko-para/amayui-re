# opcode 表对账守卫：hasHandler 与 argc vs handler 长度槽

- id: REQ-01M5ED2AD7SKV077BRVRGNE3R7
- type: req
- status: open
- parent: REQ-01M5D1G31PN9EGANRVAR753BPC
- tags: [spec, guard]

## 问题

`format/opcode-table.json` 本轮新增 `hasHandler` 列（574 条：544 true / 30 false），使分发物能判"哪 30 条执行即抛"；但**没有任何守卫**保证这一列与语料的分派表一致，也没有守卫对账 `argc` 与 handler 写的长度槽。

判据：把某条的 `hasHandler` 改错、或把某条 `argc` 改错，现有 `pnpm test` **不会红**。

## 要做什么

1. 从语料机械提取 `opcode → handler`（`pnpm tools opcodes handlers --json` 已有），断言表里 `hasHandler` 与之一致（544/30）。
2. 对 `argc` 做**抽样或全量**对账：handler 写的长度槽 `2×argc+1` 与表里的 `argc` 一致；已知例外 `0x6`（写 3 → 推进 12 字节，但 argc=2）要有**显式白名单**并把理由指向 `RES-FRAME-LOAD-FRAME`。
3. 加一条变异条目：改坏一格的 `hasHandler` ⇒ 守卫红。

## 判据

- `tools/test/spec-isa.assets.test.mjs`（或新文件）里新增用例，且 `pnpm test:mutation` 里能证明它会红。
- 白名单只有 `0x6` 一条，带理由与残差 id。
