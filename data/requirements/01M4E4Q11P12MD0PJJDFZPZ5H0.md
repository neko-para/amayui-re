# 帧模型要从**栈**改成**槽 + cur**（0x6 load-frame 的槽语义表达不了）

- id: REQ-01M4E4Q11P12MD0PJJDFZPZ5H0
- type: req
- status: done
- parent: REQ-01M4AGPP1T9A60HQYW3GX9W592
- verify: tools/test/emulator-frames.test.mjs#★★ `0x6 load-frame` 的槽语义：记录建在**指定槽 `op2`** 上，而 `cur` **一动都不动**

## 缺口

`0x6 load-frame`（`sub_41C7C0`）的逐字是**槽语义**：
```
Engine[0x5D884] = Engine[0x5D880];      ; 保存旧 cur
Engine[0x5D880] = op2;                  ; cur = **目标槽**
result = sub_40ED40(Engine, _, Engine[0x5EB54], op1);   ; ★ 在槽 cur 上建**帧记录**（局部池/头部计数/ip/flags…）
Engine[0x5D880] = Engine[0x5D884];      ; ★ **恢复** cur
```
本层的帧模型是**栈**（`Machine.frames[]`，`cur` = 栈深、由 `pushFrame`/`popFrame` 维护）
⇒ **表达不了**"往任意槽 `op2` 写记录、装完再切回原来的 cur"。

## 现状（已记账，不静默）

`0x6` 目前只做三件事：前置条件（key 非 0 只记录不抛，见另一张单）、**帧深上限 40**（越界抛，
错误信息照抄引擎那句日文）、**按 id 取脚本字节**（`system.script.load`，可观测的那部分）。
★ `sub_40ED40` **真正建帧记录**那一步**没做**，每次发一条 `logged-only`（`engine.load-frame`），
进 `[保真欠账]`；assets 守卫断言它 ≥ 4 次且信息里写明"帧记录没建"。

## 要做什么

把帧模型从**栈**改成**槽数组 + cur**：
* `slots: (ScriptFrame|null)[]` + `cur: number`；`frame` = `slots[cur]`；
* `pushFrame` = `slots[++cur] = …`、`popFrame` = `slots[cur--] = null`；
* **新增** `loadFrameAt(slot, script)` = 在槽上建记录（不改 cur）；★ 实际落地时第二个参数取**已装载的脚本**（"从哪拿字节"与"建记录"分开 —— 与 `loadScriptById`/`pushFrame` 的分工同形），措辞上原先写的 `scriptId` 由 `0x6` 自己先换成脚本；
* `snapshot()` 要带上 `cur` 与整条 `slots`（含空槽）⇒ 分区表/往返守卫要同步。

## 判据

1. 既有守卫全绿（`emulator-frames` / `state-partition` / assets 棘轮）；
2. 新增守卫：`load-frame` 之后**槽 `op2` 上真的有帧记录**（脚本名 / 局部池 / 头部计数都对得上），
   且**当时的 cur 没变**；
3. 快照往返仍然逐字节相同。


## 收口（2026-10，实现侧已落地）

帧模型已改成**槽数组 + `cur`**（`Machine.slots` / `Machine.cur`）；新增 `Machine.loadFrameAt(slot, script)` =
在指定槽上建帧记录、**不改 `cur`**，`0x6` 真的用它；`0x02 exit` 改走 `Machine.popFrame()`。
欠账**收窄**（⛔ 不是消失）：`engine.load-frame` 由 `logged-only` → `modeled`，另加一条
`logged-only` 的 `engine.load-frame-fields` 记 `sub_40ED40` 里仍无承载面的字段。
口径与依据见知识台账 `KN-01M4GZSAD4643F4M6K1V075M4D`（槽语义的取证在 `KN-01M4E548CH4C0G45171Z564G7K`，本条不重复）。
