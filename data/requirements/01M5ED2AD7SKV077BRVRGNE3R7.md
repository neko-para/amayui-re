# opcode 表对账守卫：hasHandler 与 argc vs handler 长度槽

- id: REQ-01M5ED2AD7SKV077BRVRGNE3R7
- type: req
- status: done
- parent: REQ-01M5D1G31PN9EGANRVAR753BPC
- verify: tools/test/spec-isa.assets.test.mjs#★ 指令表对账：hasHandler 与语料分派表一致；能直接读出的长度槽 = 2×argc+1
- tags: [spec, guard]

## 问题

`format/opcode-table.json` 曾缺“有无处理函数”一列，分发物判不出“哪 30 条执行即抛”；`argc` 与 handler 写的长度槽也没有机械对账。

## 本轮做了什么

1. 表新增 **`hasHandler`**（574 条：**544 true / 30 false**）；`format/opcode-table.md` 与 `pnpm tools spec describe` 的字段表同步。
2. 新守卫（`@env assets`）：`tools/test/spec-isa.assets.test.mjs#★ 指令表对账：hasHandler 与语料分派表一致；能直接读出的长度槽 = 2×argc+1`
   - ① 从 `.lst` 机械提取分派表赋值行（`tools/lib/opcodes.mjs` 的 `extractHandlers`），逐条比对 `hasHandler`；
   - ② 每个 handler 里**恰好一条**直接写长度槽的立即数（`mov dword ptr [esi+ecx*8+5D8F4h], imm`）必须等于 `2×argc+1`；
     实测 **446 / 544** 条可直接读出，**全部一致**；其余 handler 走间接写入或多次写入，不在本条守卫范围；
   - ③ 防“守卫静默缩小”：直接可读的条数 < 400 即红。
3. 变异条目（`tools/mutate-check.mjs`）：把 opcode 1 的 `hasHandler` 改成 false ⇒ 守卫必须红（已实测 `--match hasHandler` 1/1 变红）。

## ★ 更正一个前提

本单原写“有 `0x6` 的 argc=2 与长度槽 3 不一致 ⇒ 白名单”。**这是误判**：
`0x6` 的 handler `sub_41C7C0` 写的是 **5**（= 2×2+1）；写 3 的是 argc=1 那一族。
★ 2026-10-21 追加更正：写 3 的 `0x8` handler 是 **`sub_41C900`**（表项偏移 `0x0A50BC`），不是 `sub_41C8D0`
—— `sub_41C8D0` 挂在 `0x0A50B8` 上 = **`0x7`**（"长度槽写 3" 不是归属判据，`0x7` 与 `0x8` 都写 3）。
⇒ **不需要白名单**，② 的“全一致”就是字面意思。spec 与 audit 里对应的“不一致”措辞已删（`RES-FRAME-LOAD-FRAME` 已在本轮被两条成篇规则取代）。

## 判据

- 守卫绿：`node --test --test-isolation=none tools/test/spec-isa.assets.test.mjs`（2/2）
- 变异红：`node tools/mutate-check.mjs --match hasHandler`（1/1 按预期变红）
- `pnpm tools spec validate` 13/13
