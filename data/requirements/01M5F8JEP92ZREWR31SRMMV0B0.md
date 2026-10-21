# spec：写数组填充族 `0x6c fill-zero` / `0x2d8 set-array-to` 的规则

- id: REQ-01M5F8JEP92ZREWR31SRMMV0B0
- type: req
- status: done
- parent: REQ-01M5D1G31PN9EGANRVAR753BPC
- verify: tools/test/spec.test.mjs#★ 真源：data/spec/{spec,bindings,audit}.json + chapters/*.md 过全部不变量，且试点未登记 = 0
- tags: [spec, ops, array]

## 结果（2026-10-21）

`repo:data/spec/chapters/60-ops-array.md` 新增「数组填充」两条：

* `CORE-OPS-ARRAY-FILL-ZERO`（`0x6c`）：目标地址 + 个数；连写 count 个 dword，值是 `ENC(0)`（不是字面 0）；count ≤ 0 不写；越界不检查。
* `CORE-OPS-ARRAY-SET-TO`（`0x2d8`）：同上，值是 `ENC(操作数 2 的值)`（不是原值）。

配套：`bindings.json` 各一条（站点回 `.lst` 核为指令行首）、`audit.rules` 各一条（draft / handled / 残差 + falsifier）、章首计数 42→47 条规则 / 48→53 opcode。

## 边界

* 证据只用 `guard:`（`tools/test/emulator-new-array-fill.test.mjs` 的两条 ★★ 用例）；单里原引的台账 `01M4ASXQ…` 已 retracted，**没有**照抄。
* `0x2d8` 没有单独 count≤0 用例；该口径由 handler 的 `jle` + `0x6c` 同构守卫旁证，已在残差写明。

## 怎么查

`pnpm tools spec show CORE-OPS-ARRAY-FILL-ZERO` · `pnpm tools spec validate`。
