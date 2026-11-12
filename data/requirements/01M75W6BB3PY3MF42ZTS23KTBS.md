# 让 feeder 也能吃段级 override（解锁项）+ PCM 转换段 0x48D767 落地

- id: REQ-01M75W6BB3PY3MF42ZTS23KTBS
- type: req
- status: open
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- tags: [emulator-lift audio performance]

## 目标

让 **feeder 也能吃段级 override**（解锁项），并把 **PCM 转换段** `0x48D767 → 0x48D89A` 落地。

## 现状（已核）

- **"段级" = `NativeBlockTable`**：锚一个函数中段的**固定 EA**，dispatcher 在**每个块入口**按 EIP 查（`machine.cpp:1370-1411`）。
- feeder **只查函数级表**（`audio.cpp:1752-1766`），`AudioEngine` 里没有块表（`audio.h:373-379`）⇒ 段级在 feeder **静默不生效**。
- 而 `thread_mode_ = (Sdl && realTime)`（`audio.cpp:185`）⇒ **默认"真设备 + 窗口"就是 feeder** ⇒ 这是"假绿"来源。
- 块缓存 / Decoder / `X86` 都是 feeder **私有拷贝**（`audio.h:376-377`、`audio.cpp:1395-1400`、`1407-1424`）。

## 最小方案（6 处落点）

`x86.h` 加 `const NativeBlockTable* native_blocks` → `machine.cpp:319` 旁装配 → `audio.h:375` 加 `audio_block_overrides_` →
`audio.cpp:1408` 拷快照 → `runFeederGuest` 照抄 `machine.cpp:1370-1411` 的分支（在函数级之后、`get()` 之前；
`Replaced` ⇒ `insns += n` + `continue`，**不 tick 时钟**：`now_100ns_` 非原子）→ `NativeBlockOverride` 加 `feeder_ok`（opt-in）
+ `entry_insn/resume_insn`。★ 段级**不 patch 字节** ⇒ 不用重建块；跳/回由 fn 写 `EIP = 0x48D89A`。
★ **必须同单**加 `bov= / bovd= / bovf=` 到 `LIFT_AUDIO_TRACE` 的 fill 行，否则段级在 feeder 里**没有读数**（`native-ovr:` 只汇总主表）。

## PCM 段规格要点

- 乘数 **32767.0**（`.lst:412477` + 按 IEEE754 独立解出 `0x40DFFFC000000000`，**非 32768**）；低 32 位**有符号**比较。
- 每样本只写 **2 字节** + `[ebp-108h]`（**OR，⛔ 绝不重置**：它是**每次调用**一个，`0x48D70E` 清、段每通道进一次）。
- 出口还要写 `EAX=末个 clamp` / `EDX=末个 ftol2 高 32 位` / `ECX=ch` / `EDI=ns` / `EBX += 2*ch*n` / `EFLAGS(ZF=PF=1, CF=AF=SF=OF=0)`。
- x87 净不变（`fld` 一次 + 每样本 `fistp` 弹回），但**必须**做一次 `fst dword` 等价转换 —— 乘积在 ext80 恒精确 ⇒ `fmul` 不置 PE；
  **PE 来自 ftol2 那次 f32 转换，省了就状态字必红**。
- 算术：`double p = f*32767.0`（恒精确）+ `x87FromDouble` + `x87ToI64(truncate)`；
  ⛔ **不调 `nativeFtol2Sse`**（它可 per-value decline ⇒ 就得两遍 + 重叠闸门）。**一遍、升序、读一条写一条**（两遍在源/目标重叠时分叉）。
- `replaced_insns = 15*n + n_clip`（逐块数出；依赖**两臂都启用 `__ftol2_sse`**）。

## 闸门

G1 `[ebp-118h]/[ebp-108h]` 已提交 · G2 `[esi+25C4h]/[esi+26D8h]` 已提交 · G3 `1<=ch<=8` · G4 `edi<ns` · G5 源跨度已提交 ·
G6 目标跨度 `2*ch*(n-1)+2`（**不是 `2*ch*n`**）已提交 · G7 `ST(0)` 的 ext80 位型 == 32767.0 ∧ 空闲槽 ≥2 ·
G8 `[0x55F2E8]==0` 已提交 · G9 `align16(esp-0x24)+0x20` 已提交 · G10 段内无 shim。
★ **不该做闸门**三条：非 safe mode（**两种模式都要**显式 `committed()`）· 流 EOF（上游已挡）· 音频格式/通道数（段内不读设备格式字段）。

## 守卫分工

语义证据**只能**来自**真字节两臂**（assets 夹具）；pump 臂补"整条混音 WAV 逐字节 + `fills/mixed/played`"；
feeder 臂**只能**看 `bov= / guest=`。变异 3 条：乘数→32768（值表须含 `±0.5` 这类**非满幅**样本，否则被 clamp 掩盖）·
clamp 判据改无符号 · 不写 `[ebp-108h]`。

## ★ 两个假绿陷阱（报数前必须排除）

1. **量错臂**：`openSdl()` 失败会把 mode 降为 `Virtual`（`audio.cpp:173-181`）⇒ `thread_mode_=false` ⇒ pump 里"本来就能生效"。
   ⇒ 报数前必须贴 `feeder=host-thread` / `device=ok` 那两行。
2. **`ov=` 会下降**（ftol2 的函数级 hits 被段吃掉）⇒ 盯 `ov=` 会得出"没生效"的反向结论。⇒ 段级看 `bov=`，函数级看 `ov=`。

## 顺序

**一单做完**（dispatch + `bov=` + 段 + 夹具）：dispatch 单独落地时**没有会红的守卫**。
先做段级，⛔ 别退回"函数级整块换"（那要多克隆 14 个 guest call）。

## 附带发现

`platform.h` 的 `THREAD_LOCAL=空` 注释已因 feeder 成为**第二条 guest 执行流**而过期（本段对其鲁棒，但注释是错的）。
