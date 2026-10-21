# spec：写数组填充族 `0x6c fill-zero` / `0x2d8 set-array-to` 的规则

- id: REQ-01M5F8JEP92ZREWR31SRMMV0B0
- type: req
- status: open
- parent: REQ-01M5D1G31PN9EGANRVAR753BPC
- tags: [spec, ops, array]

## 一句话

`0x6c fill-zero` / `0x2d8 set-array-to` 是启动链 `INITCONFIG4` 的前两条，但 spec 里没有条目（`60-ops` 的 48 个 opcode 不含它们）。两条同族、只差"填什么值"，适合写成一族两条规则。

## 证据（已存在）

- 台账 `01M4ASXQ587J7G7E6E5R3R2Y2K` ①：`0x6c` 的逐字 —— **填的不是 0**，每个 dword 填 `Engine+0x5EC90`（= `ENC(key,0)`）；
  `op1` 是**目标地址**（取址原语）、`op2` 是个数（**≤ 0 什么都不做**）。
- `0x2d8`：同样的"地址 + 个数"，值 = `ENC(key, op2)`（`ENC` 的式子见「数据池」的编码）。
- 参考实现：旧 `apps/emulator/src/vm/ops.ts` 的 `opFillZero` / `opSetArrayTo`；
  新 `apps/emulator-new/src/handlers.ts` 的 `fillZero6c` / `setArrayTo2d8`。
- 守卫：`tools/test/emulator-new-array-fill.test.mjs` 的 `★★ \`0x6c fill-zero\`` / `★★ \`0x2d8 set-array-to\``；
  变异 3 条（填字面 0 / `<=` 多写一格 / 0x2d8 填原值），各自 1/1 红。

## 要做什么

1. `60-ops` 加一族两条：`CORE-OPS-ARRAY-FILL-ZERO`（`0x6c`）/ `CORE-OPS-ARRAY-SET-TO`（`0x2d8`）。
   正文只写：地址寻址（池 + 槽号）、连写 `count` 个 dword、值的口径（`ENC(key,0)` / `ENC(key,op2)`）、`count ≤ 0` 不做、越界不检查（引擎不检查）。
2. `bindings`：站点 = 两个 handler 的循环/编码/地址读取；`evidence` = 上面台账 + 新守卫。
3. `audit`：`handled` + 残差（越界写会踩相邻数组 ⇒ 行为未取证；浮点池按裸位模式解释）。
4. 章首 48 → 50 个 opcode（42 → 44 条规则）。

## 判据

- `pnpm tools spec validate` 13/13；新条目有 `binding.evidence`；
- 守卫会红（见上）。
