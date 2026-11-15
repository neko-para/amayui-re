# M-lift-5 · 合成 snippet PE：补 fsub/fsubp/fsubrp/fcom（4.3% 的 x87 调用）的逐位证据

- id: REQ-01M7FRY0M1RH5XCT8XD2MTFTSC
- type: req
- status: open
- parent: REQ-01M7CD1NRK9P5WKR5PJ6GW3067
- tags: [emulator-lift, lift, test]

## 缺口是什么

`x87` 里 **`fsub` / `fsubp` / `fsubrp` / `fcom`** 这四个 op 占 **4.3% 的 x87 动态调用**，但**没有逐位证据**：
它们的静态站点**全部**落在 `0x450000+`，而 `sectfix` 那份 PE **那一段没有 raw 字节**（读出来 8 字节全 0；
`.lst` 头写明它来自**另一份 dump**，`Input SHA256: 1937F0…`）。

⇒ 现在这几条只有 **3000 帧验收臂**兜底（跑到、不崩、指纹不变），**没有"两臂逐位比"的证据**。
`OBS-20261115-06` 已如实记账；`fsub` 的符号方向正是**最容易写反**的地方（本轮 `Fsubr` 的变异就是靠差分才抓到的）。

## 要做什么

做一份**合成 snippet PE**：给 `lift-gen` 一个"打字节补丁"的能力（在一份 PE 的指定 EA 处覆写若干字节），
让差分夹具可以**自己造出** `fsub m32` / `fsubp st(i),st(0)` / `fsubrp` / `fcom` 的站点，而不依赖语料恰好有那段字节。

落点估计（要自己核实，不要照抄）：新增一个构建期小工具或 `lift-gen` 的一个 `--patch <EA>:<hex>` 口径 +
3 条 CMake 链（生成补丁 PE → 以它为 `--exe` 生成模块 → 注册差分用例）。⛔ 不许让补丁能力进入产品路径。

## 判据

1. `lift-x87-diff`（或新用例）覆盖这四个 op，**两臂逐位相同**：8 个 double 槽的位型 / `top` / `tag` /
   状态字 / 控制字 / 8 GPR / EFLAGS / 1 KiB guest 内存。
2. 至少一条**变异**证明它会红（例如把 `fsub` 与 `fsubr` 的操作数对调）。
3. 合成 PE 只在**测试**里出现：产品路径的 `lift_pin` 仍是那份真 PE（`sha256` 必须对得上），
   且补丁工具**不得**被 `emulator-lift-aot` 的构建依赖到。

## 边界

- ⛔ 不要用"改语料文件"来造字节（语料只读、且那样会把证据和真源混一起）。
- ⛔ 不要为了让差分变绿而放宽判据（例如只比 `top` 不比位型）。
- 这条的收益是**证据覆盖**，不是速度；排期应低于 `M-lift-3`（图形 shim）与本单之前的欠账。
