# ret(0x05) 被实现成弹帧，而引擎是同帧就地返回（每帧自己的返回栈 帧+0x5EE04/0x5EEA4）

- id: REQ-01M4AT41GQBN6D7QE0G349NH1R
- type: bug
- status: done
- parent: REQ-01M4AGPP1T9A60HQYW3GX9W592
- verify: tools/test/emulator-frames.test.mjs#★ 两层子程序返回
- repro: tools/test/emulator-headless-logo.assets.test.mjs#REPRO(ret-0x05)
- severity: S2

## 可观测分歧

`ret`（opcode 0x05）被实现成**弹帧**，而引擎是**同帧就地返回**。逐字判据（handler `sub_41A9B0`，起始 EA `0x41A9B0`，分派表项 `Engine+0x0A50B0` ⇒ `(0xA50B0−0xA509C)/4 = 5` ✓）：

```
0041A9CE dec  dword ptr [ecx+eax*4+5EE04h]   ; 帧内「层数」−1
0041A9EE mov  edx,[ecx+edx*4+5EEA4h]         ; 从该帧的返回栈取回返回序号
0041A9F5 cmp  edx,0FFFFFFFFh / jz locret_41AA4D   ; 空 ⇒ 直接 retn
0041AA0E mov  [eax+5D898h],edx→PC            ; PC ← 脚本基址 + 序号*4
```

⇒ `ret` **不换帧、不读 `帧+0x5D8CC`、不改 `cur`**；它用的是**每帧自己的返回栈**（`帧+0x5EE04` 层数 / `帧+0x5EEA4` 返回序号表）。
而 `exit`(0x02) 才是"回到上一层帧"（读 `帧+0x5D8CC` → `Engine+0x5D884` → `Engine+0x5D880`）。

对照：`call`(0x8f) / `load-frame`(0x06) 与这**两层**结构的关系还没拿到完整判据。

## 后果（为什么它现在还没暴露）

启动链走到 `SYSTEM4.BIN` 第 1 条就停了（`0x1a8 dev_ukn` 没有 handler），**还没撞到 `ret`**（它在闭包里 8 处 / 5 份脚本）。⇒ 这是一个**已知但未触发**的分歧：一旦走到用 `ret` 做子程序返回的脚本，PC 会跳到调用者的下一条而**不是**同一帧的返回点 —— 而两类返回在大部分脚本里"看起来都对"，只会在嵌套/递归或同一帧多次调用时错开。

## 复现与收口

* **重开条件**：启动链推进到任一含 `ret` 的脚本（`CHECKCONFIG.BIN#56` 是闭包内第一处）时，行为即可观测。
* **收口判据**：模型里加"每帧返回栈"（`Machine` 上）并把 `ret` 改成同帧返回；守卫 = 一段**两层返回**的合成脚本（在子程序里再调一次子程序，两次 `ret` 落点不同），实现错了必须红。
* ★ 顺带记：本条与 ADR「地址空间」无关，是**帧机制**自身的缺口。
