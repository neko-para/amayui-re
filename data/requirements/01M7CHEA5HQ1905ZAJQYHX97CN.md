# Shape A 原型：真实函数链 lift + EA→代码派发 + shim trampoline（解释器不在环里）

- id: REQ-01M7CHEA5HQ1905ZAJQYHX97CN
- type: req
- status: done
- parent: REQ-01M7CD1NRK9P5WKR5PJ6GW3067
- verify: apps/emulator-lift/CMakeLists.txt#lift-ir-proto
- tags: [emulator-lift, lift, prototype]

## 目标

把 Shape A 的**形态**与 **shim 的利用方式**用真实 guest 代码验证掉：真实函数链 → `.ll` →
与 runtime + shim 联编成**一个二进制**，**解释器不在执行路径上**（只在同一进程里当差分 oracle 用）。

## 判据

1. **差分 0 不一致**（进守卫）：同一份初始 guest 状态（8 GPR + EFLAGS + 相关内存 + guest 栈），
   lifted 臂与解释器臂的 **8 GPR + EFLAGS 六位 + 被写内存 + 出口 EIP** 逐位相同；随机化 ≥ 200 组，
   含 `n = 0` / `n = 1` / 非对齐 buffer / 冷路径（表未建）与热路径（表已建）。
2. **每次跨函数边界 < 10 ns**（中位数，同进程 A/B）。
3. **端到端 ≥ 20×**（同一负载、同进程，lifted 臂 vs 解释器臂）。

第 2/3 项只作**读数**进 `OBS-20261114-15`，⛔ 不写成断言（换机器就变）。

## 结果（两阶段都落地，读数见 `OBS-20261114-15`）

- **阶段 A**（子单 `REQ-01M7CNWQY0WK2FBWR5GD2Q90P2`）：`sub_436D00` 的 30 条手写 lift + resume-EA 表 +
  与解释器的**混合模式**（两个建表器留解释器）+ 240 组差分 0 不一致 + `--lifted-only` 负控 exit 9。
- **阶段 B**（子单 `REQ-01M7CNWR94KV612C0EK1R2Y3WS`）：`sub_4D4B40` 的 14 条 → ③`call eax`（槽值 = 另一条
  lifted EA）与 ④`call ds:[IAT]` → `lift_shim` → `invokeById` 走**两条不同的路**；80 组差分 0 不一致。
- **读数**：`n=4096` 热路径 **≈99.6×**（阶段 A 首测 ≈101×）· 每次 trampoline 边界 **≈0.2 ns**（短载荷控制；
  `n=4096` 口径被 ±50 ns 噪声淹没 ⇒ 分辨不出）· `lift_shim` 往返 87.3 ns vs `invokeById` 79.1 ns（包装开销 8.2 ns）。

## 形态（钉住的四件事）

1. **入口 ABI**：`i32 @lift_fn_<EA>(ptr %st, ptr %mem, i32 %resume_ea, ptr %cpu)`，返回"下一个 guest EA"；
   runtime 侧一个小派发循环跑到哨兵为止。**lifted 函数之间不互相调用**（⛔ 不在 IR 里重造 dispatcher）。
2. **状态**：独立 POD `LiftRegs{u32 r[8]; u32 eflags;}`（36 B），与 `X86` 的布局差异用 `offsetof` 静态断言钉住；
   搬运**逐字段**；调 shim 前后各做一次物化/回读。
3. **shim**：`lift_shim(桩 EA, st, cpu)` 薄包装 → 既有 `ShimRegistry::invokeById`；IAT 那条路整条复用。
   ⛔ 不改 `native_overrides`。
4. **载荷**：`sub_436D00` + （解释器里的）两个建表器 + `sub_4D4B40`；都是 `AGE.EXE` 的**真字节**
   （`placeCode`，hermetic，不加载 PE）。

## 未做

块边界的定时器/输入钩子（原型只到函数出口）· SMC 写自身代码 · 含 x87/SSE 的函数 · arm64 实跑 ·
整 `.text` 与 `.ll` 生成器 · 真实负载的 dispatch 税（热循环整个在一个函数内 ⇒ 本原型测不出它）。
