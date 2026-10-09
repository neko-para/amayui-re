# 帧模型要从**栈**改成**槽 + cur**（0x6 load-frame 的槽语义表达不了）

- id: REQ-01M4E4Q11P12MD0PJJDFZPZ5H0
- type: req
- status: open
- parent: REQ-01M4AGPP1T9A60HQYW3GX9W592

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
* **新增** `loadFrameAt(cur, scriptId)` = 在槽上建记录（不改 cur）；
* `snapshot()` 要带上 `cur` 与整条 `slots`（含空槽）⇒ 分区表/往返守卫要同步。

## 判据

1. 既有守卫全绿（`emulator-frames` / `state-partition` / assets 棘轮）；
2. 新增守卫：`load-frame` 之后**槽 `op2` 上真的有帧记录**（脚本名 / 局部池 / 头部计数都对得上），
   且**当时的 cur 没变**；
3. 快照往返仍然逐字节相同。
