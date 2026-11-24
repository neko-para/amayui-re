# AOT · S7 覆盖面：不可跟踪帧恢复的间接 call（热泵 004BBAB0 / 主循环 00412290）

- id: REQ-01M86NMT0YJ9V78XS2DZZV364Q
- type: req
- status: open
- parent: REQ-01M7JHSEZVMYB4XHEN32V7V6HJ
- tags: [emulator-lift, aot, s7]

# AOT · S7 覆盖面：不可跟踪帧恢复的间接 `call`（热泵 `004BBAB0` / 主循环 `00412290`）

## 一句话

判据 4 的"可证放宽"已收（间接 `call` 后 `esp` 未知 + 只有可证帧恢复才放行；可结构化 5,343→6,411、
宿主 `call` 站点 1,572→2,967），但**热泵 `004BBAB0`（卡 `004B60C0`）与主循环 `00412290`（卡 `004E8C21`）
仍未解锁** —— 那两条间接 `call` 之后**没有任何可跟踪的帧恢复**，"可证"这一档结构上管不到它们。
上一轮估的 85–105 ms 因此没拿到。

## 先回答的问题（答不上来就不要动判据）

`004B60C0` / `004E8C21` 里的间接 `call` 之后，**caller 的宿主帧到底还在不在**？

- **在**（callee 一定回到本调用点，例如 `lift_dyn` 的返回检查保证）⇒ 可以放宽成"间接 `call` 之后按
  callee 的返回行为建模"；
- **不在**（真尾调用 / callee 可能用 `ret imm16` 不回到本点）⇒ ⛔ 不能放宽；那条边的宿主 `call` 前提是
  callee 可结构化，属另一条依赖链。

## 判据

1. **形状级差分**：扩 `repo:apps/emulator-lift/tests/s7_indirect.cpp` 的矩阵，覆盖"间接 `call` 后无帧恢复"
   的两种形态（正常返回 / 非本点返回），与解释器（`Machine`）**逐位相同**；先红后绿（同上一单的做法）。
2. **目标两处变成 structured**：生成期 `[S7 criteria]` 里 `004BBAB0` 与 `00412290` 可结构化。
3. 五项指纹（`exit` / `stop @` / arena / `table misses`）逐位同；`ctest` 全绿；
   变异：把新判据反转 ⇒ 差分用例红。
4. 读数：`iter`（派发次数）↓、`insns/iter` ↑、`lift` ns/guest 指令 ↓；点击大帧 / FIELD 装载帧 wall。

## 边界

- ⛔ 不许用运行期 EA→函数指针表 / native override（`repo:tools/test/aot-arm-shape.test.mjs` 的禁用词）。
- 本单是 `REQ-01M81PTTC7XBK365YGQNQTTXWA`（判据 4 的可证放宽）之后的**下一层**，不重复它；
  也不重复 `REQ-01M81W63PV0JRRX8QZY7ZGEGTF`（`leave` 模型，已 dropped：无错编译）。
- 依赖：本树下"派发密度 / 块合并"那一单先把**跨函数 call/ret 占派发多少**定出来 —— 若那块占比不大，
  本单优先级应下调。
