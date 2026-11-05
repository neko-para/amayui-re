# emulator-lift · 局部 native 化（guest 函数级 override）可行性与性能试点

- id: REQ-01M6NZSAAD0WCNBE3RKMFF84R5
- type: spike
- status: open
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- tags: [emulator-lift, performance, native-override, spike]

## 一句话
在**保留原始 x86 控制流**的前提下，把少数已取证的 guest 函数替换成宿主 C++ 实现（函数级 native override），用差分测试验证语义、用 A/B 测启动/帧时间；先判断它是否值得扩展，不直接上全量 JIT/AOT 或 `emulator-cxx` 式重写。
## 背景（2026-11-05 实测）
- RelWithDebInfo（`-O2 -g`）到第一帧 `Present`：**9.83 s / 707,597,230 insns / 约 72 MIPS**。
- `sample`：`Machine::run` 占 **95%**；`execInteger` 48%、`execString` 8.5%、`X86::readMem`/`readOp`/`eaOf` 合计约 26%、`g_access` 6.6%、`ShimRegistry::invoke` 5.1%、`BlockCache::get` 5.2%（inclusive，互相重叠）。
- `--until ea`：0→`_WinMain` 0.10 s；→`Direct3DCreate9` 1.97 s；→`ReadFrameTex`/`GetBackBuffer` 3.43 s；→首帧 9.83 s。约 **65%** 在资源/字体/纹理装载的 guest 代码里。
- 第一帧之后 100 帧约 40.6 s（约 **0.31 s/帧**），其中两笔全屏 `DrawPrimitive` 各写 ~919k 像素；软光栅是独立问题。
- 现有边界：native 只到 Win32/GDI/D3D shim；guest 游戏逻辑、CRT、字符串、数学仍逐条解释。

## 目标

1. 做一个**函数级 native override** 的可运行原型：在 guest 函数入口拦截，按 guest ABI 调宿主 C++，返回后恢复 `EIP/ESP`；未覆盖路径回退解释器。
2. 建立**差分测试**：同一输入下，override 与解释器的寄存器 / 内存 / 帧哈希一致；不一致必须能定位。
3. 用真实 boot 的 A/B 测量回答一个决策问题：**只替换少数热点函数，能否带来 ≥15% 的首帧时间收益？**
4. 如果收益不足，明确记录“停止扩展”的结论，不把资源投进半成品 JIT/AOT。

## 非目标

- 不做全量 x86→ARM64 JIT/AOT，不做 `emulator-cxx` 式引擎重写。
- 不改 guest 的 `EIP` 级执行模型（仍然是原始字节 + block cache）。
- 不覆盖 SEH / 异常展开 / 自修改代码 / 动态生成代码；这些必须回退。
- 不把宿主 C++ 对象 / 指针 / 异常泄漏进 guest 状态。
- 不把 `--game-dir` 当可写目录；沿用现有写覆盖层。
- 不以“优化 shim”替代本试点；本试点只回答 guest 函数级 native 化的收益。

## 方案（试点）

- 新增 `GuestOverride` 表：`guest_ea → native_fn`，每项带签名/栈清理/参数个数/是否 `__thiscall` 等元数据。
- 拦截点优先选 **dispatcher 在 block 入口检查**（不 patch guest 字节，天然避开 SMC / 代码页写保护）；若性能不足，再评估“入口 jmp 到 trampoline”。
- `native_fn` 接收 `X86&` / `ShimCall` 风格参数；只能通过 `GuestMemory` 读写 guest；返回后由框架按 ABI 设置 `EAX`、`ESP`、`EIP`。
- 覆盖候选必须由**真实 profile / 覆盖率**选出，不先猜：
  - 先加 guest-EA 计数（或利用现有 block/EA 计数）找出前 N 个热点；
  - 优先 CRT 叶子函数：`_memcpy` / `_memset` / `_strlen` / `_strcpy` / `_rand` / 定点数学；
  - 若热点是内联的 `rep movs` / `rep stos`，没有单一函数入口 ⇒ 记录为“函数级 override 不适用”，改走指令快路径，不在本单硬凑。
- 每个 override 默认关闭，用显式开关启用（例如 `--native-overrides=<list>`）；默认行为保持不变。
- 先做 3–5 个函数，不做批量迁移；每个函数都要有独立可关的开关与差分用例。

## 判据（spike 的产出）

- **能力**：至少 1 个真实 guest 函数通过 override 执行，且返回后栈/返回地址/被调用方保存寄存器与解释器一致。
- **语义**：同输入、同 `--max-frames 1` 的前后两次运行，`Present` 的像素哈希、`insns` 结构（允许 override 改变 insn 数）和 guest 内存检查点一致；不一致有最小 repro。
- **回退**：未知 EA / 未覆盖函数 / 动态代码 / SEH 路径仍走解释器，不得 trap 或静默跳过。
- **性能**：3 次运行取中位数；首帧时间相对基线 **≥15%** 才建议扩展；<15% 记录“收益不足，停止”。
- **回归**：`ctest` 全绿；现有 `boot-trace` / `d3d9` / `d3dx9` / `gdi` / `file-path-ext` 不红。
- **可观测**：日志能打出 override 命中次数、回退原因、被替换 EA；不把“看起来快了”当证据。

## 测试计划（用户后续组织）

1. **pure 夹具**：假 guest 函数（`cdecl` / `stdcall` / `__thiscall` 各一）→ override 返回；断言参数、返回值、`ESP`、返回 `EIP`、被调用方保存寄存器。
2. **差分夹具**：同一段 guest 代码分别走解释器 / override；比较完整寄存器快照、关键 guest 内存、`Present` 像素哈希。
3. **回退夹具**：override 表里没有的 EA、函数中间入口、写代码页后的 EA ⇒ 必须走解释器或明确失败。
4. **A/B**：同一 `--appdata`、同 `--max-insns`、`--max-frames 1`；基线 vs override，3 次中位数；记录首帧时间、insns、MIPS、override 命中。
5. **帧时间**：`--max-frames 100` 记录首帧后帧成本，区分“启动加载”与“软光栅”。

## 边界 / 风险

- 函数边界可能不唯一（chunk / 多入口 / 尾调用）；只覆盖已确认的入口。
- guest `flags` 不是 C ABI 的一部分；若函数依赖 flags 返回，必须单独处理或回退。
- 有 SEH / C++ 异常 / 回调 / TLS 边界的函数先不做。
- 宿主 C++ 不得持有 guest 指针；guest 指针保持 32 位。
- `setjmp/longjmp`、`alloca`、动态栈、内联汇编函数先不做。
- 若热点主要是解释器基础设施（`g_access`、block cache、operand helper），函数 override 不会解决；那属于解释器热路径优化，另开单。
- 本单不承诺最终可玩性能；只产出“是否值得继续”的结论与数据。

## 落点（预期）

- `apps/emulator-lift/src/runtime/`：override 表 / 调度接入 / 差分快照。
- `apps/emulator-lift/tests/`：override ABI / 差分 / 回退夹具。
- `apps/emulator-lift/src/main.cpp`：显式开关与日志（不改变默认路径）。
- 具体文件在试点开始时按现有 `Machine` / `ShimRegistry` / `X86` 接口定，不预先造抽象层。
