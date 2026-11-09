# emulator-lift · M1 收口暴露的技术债（conformance 恒等判据 / movsd 碰撞 / SSE 对齐守卫 / MXCSR 覆盖）

- id: REQ-01M6ZBAKJW9ENGET93HQAQVE7E
- type: req
- status: open
- parent: REQ-01M3TC7BK0J9TBQKGGWQ35A3NR
- tags: [emulator-lift, debt]

## 一句话
本轮收口 M1 时暴露的**不属于**那三张单（locale / SSE）的技术债，逐条都是可判定的：

1. **conformance 的"恒等夹具"判据几乎恒假**：`tests/conformance.cpp` 的 `Snapshot::operator==` 已把 `eip` 移出比较，
   但夹具末尾的哨兵 `ret` 必然把 `esp+4` ⇒ `memHash` 必变。实测：把 `xmmHash` 从比较里去掉，236 条用例一条都不红。
   ⇒ 要真守住"夹具什么都没做"，得让 runner 把哨兵 `ret` 的栈效果排除掉（或用例级 opt-out）。**修法可判定**：造一条
   "setup/verify 都空"的假用例，它必须被这条判据判红。
2. **`movsd` 助记符碰撞**：`F2 0F 10`（SSE2 标量 double）与串操作 `movsd` 共用 Zydis 助记符 ⇒ 归一化成 `Op::Movs`，
   真撞到会**静默**按串操作执行。语料 0 条（`.lst` 无 `xmm` 操作数）⇒ 重开条件 = 出现 `xmm` 操作数；
   修法是解码时按操作数形态分派。
3. **SSE 对齐违例没有守卫**：`movaps`/`movdqa` 的 m128 未 16 字节对齐 ⇒ 现在走 `unimplemented()` 响亮失败，
   但 conformance 的 `run()` 要求到达哨兵，表达不了"响亮失败" ⇒ 这条只有实现没有守卫（`tests/dispatch.cpp` 是合适落点）。
4. **MXCSR 只建模 IE**：DE/ZE/OE/UE/PE、掩码、RC、异常投递都没建模（没有 `ldmxcsr`/`stmxcsr`/`fxsave` 消费者）；
   `AudioEngine::Context` 的 save/restore 也没有 `mxcsr`（feeder 有独立 `X86` 且不跑 SSE 浮点 ⇒ 今天无可观测差异）。

## 判据
- ① 修好后：假用例（setup/verify 都空）在 `conformance` 里**红**；
- ② 修好后：`F2 0F 10` 形态有独立解码路径 + 一条用例（或明确登记"与串操作同形、语料 0 条"的裁决）；
- ③ `tests/dispatch.cpp` 有一条"未对齐 movaps ⇒ 响亮失败"的用例；
- ④ 只在真要跑 `ldmxcsr`/`stmxcsr` 时才有意义 ⇒ 可维持现状并登记重开条件（本条可先只记账）。

## 不做什么
- ⛔ 不为了②③去扩语料 / 改 `.lst`；语料没有就按"重开条件"记。
- ⛔ 不把 ① 修成"跳过恒等判据"（那等于删守卫）。
