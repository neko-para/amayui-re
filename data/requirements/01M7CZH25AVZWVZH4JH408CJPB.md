# M-lift-1 · 第一条可执行线：entry → TITLE（判据 = handoffs==0 + 与解释器臂可比项相等/observation；TITLE = 人工确认）

- id: REQ-01M7CZH25AVZWVZH4JH408CJPB
- type: req
- status: done
- parent: REQ-01M7CD1NRK9P5WKR5PJ6GW3067
- done_reason: 判据 1/3/4 有机械读数 + 人工确认（OBS-20261115-01/02）；判据 2 按用户裁决从'逐位一致'改为'可比项相等 + observation'（wall clock 下逐帧像素对照不可行）。能力证据：emulator-lift-aot.exe 3000 帧 exit 0 / misses 0 / 帧 3000 即 TITLE。⛔ 按用户口径不新增 CTest。
- tags: [emulator-lift, lift, milestone]

## 形态（实际落地的，与 `REQ-01M7CHE8HBPVEYTNSD05SJSW47` 的形状 A 一致）

**独立 target** `apps/emulator-lift/src/aot/` + `emulator-lift-aot.exe`：⛔ 不 `include` `machine.h`、
⛔ 不构造 `Machine`（`cpu.machine == nullptr`），自己实现 `HostServices` 装配宿主服务。
解释器只在测试构建里；产品二进制的执行路径上**没有**解释器（`machine.cpp.obj` 已不在 AOT 链接面，见 F1 读数）。

链路：`lift-gen`（构建期 host tool）读 PE + `aot/closure.txt`（入库件，3,277 个 census 推导根）
→ 静态闭包发现（**五类**：`call rel32` 目标 / 写内存 `mov` 的 imm32 / 非可执行节的代码指针 dword /
可执行节里"已是块首"的跳表值 / 可执行节里"尚未解码"的跳表值）
→ `lift_aot.ll` → `clang -c -O2` → `.obj` → 链进 `emulator-lift-aot`。
构建期硬闸：闭包五条判据（格式 / 严格升序 / 可解码 / 在可执行节 / `entry` 必在）在**生成那一趟**里做
（`--closure-gate`，2026-11-16 起不再单独跑一次 `--check`）；`.ll`/`.obj`/派发表/pin 落 `out/`（⛔ 不入库）。
**执行源被 pin**：`lift-gen` 把所读 PE 的 `sha256` + `entry` 写进 `lift_pin.h`，AOT 启动前核对，不一致 `exit 2`
（`--exe` / 环境变量只是诊断口径，⛔ 不能换成另一份 PE）。

## 判据与凭据

1. **`handoffs == 0`**：派发面只有 lifted / shim 两类，没有"把 EA 交回解释器"的路径；
   读数 `misses=0`（117,715,390 次查找，0 次落到解释器）。见 `OBS-20261115-01`。
2. **与解释器臂一致**：⛔ 原判据"`--clock=virtual` 下逐位一致"**作废**（用户裁决：总是 wall clock）。
   现行口径 = **可比项相等 + observation**：同一 `--max-frames` 档 `stop @ 00498AE4` 同址、arena 三数逐值相同、
   **frame 1 窗口像素 fnv64 逐位相同**（`0xebf024febe998b05`）；逐帧像素对照当前**做不到**（wall clock 下每帧指令数随墙钟漂，
   只有第 1 帧可比）。见 `OBS-20261115-01` 的"口径警告"。
3. **没有硬停**：3000 帧 `exit 0`（有/无 native override 两档），无 `_Exit(3/4/9)`、无 `llvm.trap`、无预算超限。
   ★ 2026-11-16 更正：第二档（"有 override"）**已不可重跑** —— AOT 臂的 `--native-overrides` 机制已整条删除
   （理由见 `docs/03-emulator/defects.md#BUG-25` 的"裁决"段）。第一档（缺省、无 override）的读数仍然成立，
   而且**缺省档就是判据口径**（`AGENTS.md` §12.5）。
4. **抵达 TITLE** = 人工确认 + 一条 observation：`OBS-20261115-01` 的帧 3000 就是标题画面
   （标题 logo + Game Start / Load Data / Eushully-chan Room / Option / Quit + `(c)2017 Eushully` + FE01–FE05）。
   ⛔ 不做成 CTest（业务正确性不作判据，`AGENTS.md` §5）。

## 为达成它有的件（都已落地）

- **生成器**：入口 = resume EA；分量内 `br` 直跳；`call` / `ret` / `jmp reg` 回 trampoline；`retn imm16`；
  x87 / `fs:` / 串操作 / `Div` 等**未 inline 的 op 走单指令宿主原语 `lift_one`**（正确性优先，inline 化只由 profile 驱动）。
  ★ 2026-11-16 起：`lift_one` 调用点已归零（S7 结构翻译 + 各 op 的 LLVM 发射），闭包另加**第六类**——跳表识别
  （前置 bounds check 反推表长 + 逐项读兜底；表区间不参与解码、表项在建块前成为 leader）。
- **闭包五类发现**（见上）；根解不出/越界 ⇒ `exit 4`；派生目标可丢（越界/解不出/重叠 分开计数 + 样例）；孤儿入口 = 0。
- **派发表**：31,607→ 现在 33,509 项（2026-11-16 读数，`lift_pin.h` 的 `LIFT_AOT_PIN_ENTRIES`；⛔ 会随闭包改动漂，
  查真值看那份生成物）；已改**开放寻址哈希**（avg 探测 1.0036，装载因子 0.256，槽表在 BSS ⇒ exe 不增）。
- **执行循环**：块边界定时器 / SEH 推进 / 三个合成 EA（续延 `0x7FFEF008` 命中过 21 次）/ `--max-frames` 闸门 /
  `--skip-logo`（一次性 latch，钩子 `0x004BAA33` 写 `T_Command+0x5EB5C=0`）。
  ★ 2026-11-16 起：`native override 分派` 这一段**已从 AOT 臂删除**（`--native-overrides` 现在是未知参数 ⇒ `exit 2`）；
  AOT 的执行来源只剩两个：**IR 与 shim**。
- **真窗口**：`SdlWindow` + GPU 光栅（headless 也建隐藏窗口 + GPU，与解释器同口径）+ 事件泵 + `stopRequested`。

## 未做（后续）

- **性能**：AOT 在**纯计算段比解释器快一倍**（18.7 vs 37.4 ns/条），但**加载/渲染段慢 1.66–1.81×**
  （块变短 + 派发 + `lift_one` 往返）；「分量融合 / `call`+返回点合并 / `ret` 校验后 host-ret」都还没做。
  ★ 2026-11-16 更正：那三件里 `lift_one` 往返与 host-ret 已经做了（见上）。
- ★ **2026-11-16 更正（覆盖本行旧文）**：旧文写"`sub_48B090`（23.2% 指令）需**嵌套 guest 执行器**；
  `sub_414AC0`（7.6%）需段级入口进派发表"——**这两句都不成立**，已核实：
  两者**都在派发表里**（AOT 里就是 lifted 代码），而且 guest 自己就会调 `sub_48B090`
  （ACIF 链 `0x49E9D0 → … → 0x49ED2B`（`[eax+1Ch]` = `+0x1C` 的 UploadImage 槽）`→ 0x49ED33`）。
  "23.2%" 只是解释器侧的**指令占比归因**，不是"必须被 override"。
  ⛔ **不要照旧文去给 AOT 补 override / 嵌套执行器**：那套机制已从 AOT 臂整条删除，
  且 `tools/test/aot-arm-shape.test.mjs` 把它的 33 个标识符列为**禁用词**（装回去必须显式改那张表）。
  证据：`.tmp/aot/nested/analysis.md`。
- 输入注入 · 音频/影片真实时序 · FELLOW / ADV / SAVE 等复杂路径 · SMC · arm64 实跑 · 整段性能验收。
