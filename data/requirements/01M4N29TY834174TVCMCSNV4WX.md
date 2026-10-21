# 配置的键：本层取**操作数的值**，引擎取**操作数序号/表下标**（同一指令两个键；改它会推进启动链 ⇒ 本轮只开单）

- id: REQ-01M4N29TY834174TVCMCSNV4WX
- type: bug
- status: dropped
- parent: REQ-01M4B248NB8FWDN61E2NMK8N6V
- repro: tools/test/emulator-engine-scalars.test.mjs#★ 配置的整型两条：**本层把键取成操作数的值**
- severity: S2
- dropped_reason: 缺陷对象 apps/emulator/src/vm/ops.ts 已废弃；apps/emulator-new 的 configKey 直接取操作数序号（type 3/5），不存在「取操作数的值」的口径 ⇒ 分歧在新实现里不存在。
- tags: [engine emulator vm]

## 缺陷（可观测分歧）

**引擎逐字**：`load-int`（`sub_42DF40`）在 `0x42DF6F push 1` + `call sub_418A30`；`save-int`（`sub_434F60`）在 `0x434F8F push 1` + `call sub_418A30` —— 两处传的都是**立即数 `1` = 操作数序号**。
`sub_418A30`（`0x418A30`）三条臂（`0x418A5B` type 3 / `0x418A60` type 6 / `0x418A65` type 12）**返回的都是表下标**（type 3 = 描述符自带的数；type 6/12 = `(表项 − Engine+0x5D800) >> 2`）⇒ 键 = `"\x03" + %8.8x(序号)`。

**本层实现**：`apps/emulator/src/vm/ops.ts` 的 `configKeyIntByValue(value)` 把**操作数的值**放进键里。
⇒ `SYSTEM4.BIN` 的 `load-int (global-int 5)`：引擎算键 `\x03 00000001`，本层算键 `\x03 00000000`（`global5` 当时是 0）—— **同一条指令、两个不同的键**。

## 复现（能红的）

`tools/test/emulator-engine-scalars.test.mjs` 的用例 **`★ 配置的整型两条：**本层把键取成操作数的值**（引擎取的是序号/表下标 —— 逐字见上）、查不到 ⇒ 0、写回是 lvalue`**：它把"本层用的是值"这条**逐字断言**下来；`tools/mutate-check.mjs` 里那条"值 → 序号"的变异就是它的重开条件 —— 一旦有人把实现改成引擎口径，该用例当场红。

## 为什么**不在本轮**改（这是有意的延后，不是遗漏）

键从"值"改成"序号"会**真的改变启动链的分叉**：`SYSTEM4.BIN` 里 `load-int (global-int 5)` **紧挨**
`jcc (global-int 5)`，而 `save-int (global-int 5)` 存的键在"序号"口径下**跨运行稳定** ⇒ 改对之后第二次运行会**第一次**读到上一次存下的那条 ⇒ 脚本会走 `LOADCONFIG` 支。
按用户限制「**不实现新 opcode、不推进启动链**」，这一步不在本轮；且它是**替换被证伪前提**的那件事（见 `REQ-01M4B416EKPVZWHFWT283C1T3Q` 的 `dropped_reason`）。

## 收口判据

改成引擎口径（键 = 操作数序号 / 表下标）+ 更新那条用例的声称（从"本层近似"改成"引擎口径"）+ 重跑启动链棘轮断言。
★ 别顺手把它和"取值原语 `sub_41BF50` vs `sub_418A30`"那条混成一条：那是**另一个**欠账（哪些 type 上两条原语一致**没有取证**）。
