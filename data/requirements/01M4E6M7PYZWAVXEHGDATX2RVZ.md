# 0x88 的两条条件副作用未建模（写常量 1 / 清 bit27；与 0x71 置位成对）

- id: REQ-01M4E6M7PYZWAVXEHGDATX2RVZ
- type: req
- status: done
- parent: REQ-01M4AGPP1T9A60HQYW3GX9W592
- verify: tools/test/emulator-engine-scalars.test.mjs#★ `0x71` 置 bit27 ⇒ `0x88` 能把它清掉（位状态真的变；记录是 `modeled`）
- done_reason: 落点：知识层新表 `ENGINE_SCALAR_BITS`（`packages/age-format/src/engine/layout.mts`，3 条：`0x88` 真支写常量 1 / 假支清 bit27、`0x71` 置同一位，各带 `dword`/`value`/`mask`/`when`）+ 模型 `apps/emulator/src/vm/ops.ts` 的 `applyScalarBits`（`scalarHandler` 与 `forwardHandler` 共同调用，经 `EngineScalars.setBits`/`clearBits` 真的落状态）。收口凭据 = 成对用例（`0x71` 置位后 `0x88` 能清掉，位**真的变**）；另有一条**语料逐字锚**用例（`tools/test/emulator-engine-scalars.assets.test.mjs`，三条 EA `0x41FAE4`/`0x41FAF0`/`0x41EE8D` 的助记符由表里的 `dword`/`mask` 现算）。台账：`KN-01M4GYTBDW7S344D4F032M650J`（accepted，`Emulator.vm`），它 replaces 了那条"只记录不建模"的 `opcode/0x88`（撤回记录 `KN-01M4GYT4464A124R6E4C1F5X7Z`）。★ 未声称：这一位**意味着什么**仍未取证 —— 本层只落状态、不给含义。

## 缺口

`0x88`（handler `sub_41FAB0`，argc 1）有**两条条件副作用**：
```
if (op1) this[122368] = 1;            ; 写常量 1
else     this[174801] &= ~0x08000000; ; 清 bit27
```
它们**不在** `ENGINE_SCALAR_WRITES` 里（形态不同：一条写常量、一条是**位清除**），
本层目前只发 `logged-only`（`system.engine.scalar.bits`）。

★ 同一位的另一侧：`0x71`（`sub_41ED80`）**置** `Engine+0xAAB44` bit27，`0x88` **清**它
（两条台账条目正文互指）⇒ 这是一个**成对的状态位**，而本层两处都没真正建模它。

## 要做什么

1. 把"写常量"与"位清除"这两种形态**登记进知识层**（`ENGINE_SCALAR_WRITES` 目前只有
   `op1`/`op2`/`bool(op1)`/`bswap24(op1)`/`const`；位操作要新形态，或单列一张"位副作用"表）；
2. 建模后要能回答"这个位现在是 0 还是 1"（现在本层答不出来：只记录、不落状态）；
3. 判据：一条守卫能证明"`0x71` 置位后 `0x88` 能把它清掉"（状态真的变了，而不是只多了两条日志）。

## 为什么值得进树

`ops.ts` 的注释曾声称"这两条已登记进需求树"，而树上**没有**点名它们的节点 —— 这正是
"注释声称有登记、实际没有"的典型（与台账覆盖率那次同源）。
