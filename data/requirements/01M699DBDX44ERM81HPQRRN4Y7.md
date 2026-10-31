# 数值族 0x52–0x59 / 0x5a–0x5f 补可执行守卫（32 位截断 / 符号 / 位数掩码 / 比较方向）

- id: REQ-01M699DBDX44ERM81HPQRRN4Y7
- type: spike
- status: dropped
- parent: REQ-01M674XHSX2HSFWSKDWZV2C63D
- dropped_reason: 内容（数值族可执行守卫）已并入交接单 REQ-01M5BK8QMQYK7XQG707YVMQ202 的 emulator 待做项；按用户口径不单独留单。
- tags: [spec, residual]

## 还缺什么

`0x52`–`0x59` / `0x5a`–`0x5f` 的**可执行守卫**。本轮已把语义收口（正文已把失败路径、32 位截断、移位位数掩码、比较方向与有符号性写死，EA 锚在 `data/spec/bindings.json`），剩下的只是"会让实现写错当场变红"的用例 —— 本轮不动实现面（`apps/emulator*` 另起一轮）。

对应残差：`RES-OPS-NUMERIC-ARITH-EXC`、`RES-OPS-NUMERIC-COMPARE-GUARD`。

## 入口 EA

- `0x42c690`：`imul edi, eax`（`0x52`，32 位截断）
- `0x42c6d0` / `0x42c6d1`：`cdq` / `idiv edi`（`0x53` 有符号除，**没有除零检查**）
- `0x42c711`：`idiv` 之后取 `edx`（`0x54` 的余数）
- `0x42c780` / `0x42c7c0`：`and edi, eax` / `or edi, eax`（`0x56` / `0x57`）
- `0x42c802`：`sar eax, cl`（`0x58`，算术右移）
- `0x42c852`：`shl eax, cl`（`0x59`）
- `0x42c8a2` / `0x42c8a4`：`cmp eax, edi` / `setz dl`（`0x5a`）
- `0x42c8f2` / `0x42c8f4`：`0x5b`，`setnz`
- `0x42c942` / `0x42c944`：`0x5c`，`setl`（**有符号** `<`）
- `0x42c992` / `0x42c994`：`0x5d`，`setle`
- `0x42c9e2` / `0x42c9e4`：`0x5e`，`setnle`
- `0x42ca32` / `0x42ca34`：`0x5f`，`setnl`

## 已经知道什么

- 七条算术的读写序（先第 3 个、后第 2 个）；乘法 32 位回绕；有符号除、余数符号随被除数；移位位数取第 3 个操作数的低 5 位；`sar` 是算术右移。
- 六条比较的**方向**（`eax` = 第 2 个、`edi` = 第 3 个）与**有符号性**（`setl` / `setle` / `setnle` / `setnl` 都是有符号条件）。
- 除零与 `INT_MIN ÷ −1` 会触发处理器异常、引擎不做检查（正文已写；宿主如何呈现不在本版）。

## 还缺什么（具体）

`apps/emulator-cxx` 的顶层 CTest 守卫：`0x52`（溢出回绕）、`0x53` / `0x54`（负除数、`−7 ÷ 2`、`−7 mod 2` 的符号）、`0x58`（负值的算术右移）、`0x59`（位数 ≥ 32 的掩码）、`0x5a`..`0x5f` 每条的镜像与负值各一例。除零与 `INT_MIN ÷ −1` 两条留待真机观测。

## 判据（什么算做完）

- `apps/emulator-cxx` 的顶层 CTest 里每条 opcode 至少一个用例，且把实现改坏（`sar`→`shr`、有符号比较→无符号、`0x5c` 与 `0x5e` 对调、移位位数 clamp 到 31）⇒ 对应用例**当场红**；
- 给 `tools/mutate-check.mjs` 加一条变异；
- 两条异常路径补一份真机观测记录（或明确写"仍缺真机"）。
