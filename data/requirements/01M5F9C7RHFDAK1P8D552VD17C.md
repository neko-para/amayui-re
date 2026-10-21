# spec：写数组寻址族 `0x61` / `0x12c` / `0x64` 的规则（含地址算术口径）

- id: REQ-01M5F9C7RHFDAK1P8D552VD17C
- type: req
- status: done
- parent: REQ-01M5D1G31PN9EGANRVAR753BPC
- verify: tools/test/spec.test.mjs#★ 真源：data/spec/{spec,bindings,audit}.json + chapters/*.md 过全部不变量，且试点未登记 = 0
- tags: [spec, ops, array]

## 结果（2026-10-21）

* `repo:data/spec/chapters/40-pools.md` 新增「数组地址算术」（`CORE-POOL-ADDRESS-ARITH`）：地址 = 池基址 + 元素宽度 × 下标（数值 / 指针 4、字符串 28）；指针槽不编解码；**只规定关系、不规定地址数值**；越界 / 未对齐是未定义行为（⛔ 不 clamp / 不补 0）；与配置章的键换算共用。
* `repo:data/spec/chapters/60-ops-array.md` 新增三条：`CORE-OPS-ARRAY-LOOKUP`（`0x61`）/ `CORE-OPS-ARRAY-LOOKUP-2D`（`0x12c`，下标 = 行 × 行宽 + 列）/ `CORE-OPS-ARRAY-COPY`（`0x64`，块首 = 代码区起点 + 4×raw，首 u32 = 个数，从 +4 起逐格编码）。
* 三条都写"不检查越界"；`bindings` / `audit` 同批，章首计数 48→53 opcode（与 #1 同批）。

## 边界

* 证据：`0x61`/`0x12c` 用 accepted 台账 `KN:01M4G4N4W34T040X434N7M4M43` + pointer 守卫；`0x64` 只有 pure 守卫（单里原引的 `01M4ASXQ…` 已 retracted）。
* 单里说"`55-config` 明写地址数值不规定"这句**现在不在**原文：现文是"键下标只在地址落在该族全局池内时有定义"。本条按 `00-overview` + `55-config` + 新 40-pools 规则三处口径写，已订正。

## 怎么查

`pnpm tools spec show CORE-POOL-ADDRESS-ARITH` · `pnpm tools spec validate`。
