# 批 R1 · 引擎通用数据区域（global 池 / local 池 / frame / 纯数值指令）重写核验 + 模拟器建模

- id: REQ-01M48DJ3PK48CXQXAV1M3EEQNR
- type: req
- status: doing
- parent: REQ-01M3TCCVC01AWFEMP6GJM96KQ4
- order: 10
- tags: [engine ledger emulator]

## 范围（★ 只管"引擎体自身存储的通用数据区域"）
**global 池** · **local 池** · **frame 概念** · **所有纯数值指令** · **迭代系统**（`EA → 指令` 的解析与迭代）。
**明确不含**（用户口径）：❌ 引擎体上基于 offset 的变量；❌ **整体执行流程**；❌ **循环副作用** —— 不处理，但遇到就登记。
★ 流程（不许在巨大反汇编文件里漫游）：① context packet（机械可复现）→ ② 只解码 + 操作数类型普查（零引擎语义）→
③ 帧/池布局与公式（不取值）→ ④ 单池取/存（先 int，须含 key/DEC）→ ⑤ 纯数值指令族 → ⑥ 迭代系统；packet 落 `.cache/`。

## 已完成的点（细节在台账与守卫，本单不复述）
* **① 机械提取器**：`EA → 行号 → 有界上下文`、`--cases`；守卫 `disasm.test.mjs` / `disasm-extract.test.mjs`；修 `peOffsetOf` 的 VA/RVA 口径。
* **② 操作数类型普查**（零引擎语义）
  | 原语 | EA | 跳转表 | case 数 | 缺席 |
  |---|---|---|---|---|
  | 取址 | `0x42AEA0` | `jpt_42AF16` | **12**（3..14） | 无 |
  | 读值 | `0x41BF50` | `jpt_41BF99` | **14** | **case 8** |
  | 写值（值来自整型） | `0x42B4B0` | `jpt_42B51F` | **10**（3..12） | **cases 5,8,11** |
  （写值（值来自浮点）= `0x42BA00`；口径真源 = 台账 `Engine+0x41BF50|0x41C300|0x42B4B0|0x42BA00/operand-type-table-and-primitives`。）
  操作数基址 = **帧+0x18**、**每个 8 字节**、opcode 在 `[第一操作数−4]`；守卫 `engine-operand-types.test.mjs`。
* **③ 帧布局**：帧内 `[0x5D880, 0x5D918]` 共 **35 个槽**（其中 `≥ 0x78` 的 **5 个**）；
  `0x5D8F8/0x5D8FC/0x5D900` 是**三个 per-cur dword 格**（`edx = 30*cur` ⇒ 净步长 **0x78**），与"帧步长 0x78"不冲突
  （冲突来自 **索引步长 0x78** vs **记录下界 ≥0x8C**）；已撤回两条错读法（"帧记录尾槽" / "`imul eax,84h` ⇒ 记录 0x84"）。
  守卫 `engine-frame.test.mjs`（6 条 / assets）+ `emulator-model.test.mjs` 帧族。
* ★ **本轮补的三条帧区事实**：① `+0x84`/`+0x88` **访问普查**（`5D904h` 11 处 / `5D908h` 10 处）⇒ 索引形态读点分布 **4 / 3 个不同原语**，
  并显式更正一条旧记录的错归属（`replaces`）；② **"扁平 vs `*8` 索引"不能当判据**（同一格两种形态都在）；
  ③ 帧尾三格**只写不读、没有读者**（全语料恰好 6 处引用：建立 4 + `_memset` 1 + 释放 1）。
* ★ 顺带修 `project()`：被 `replaces` 指向的记录原先 `effective` 没变成 `retracted`。

## ★ 两个已记录问题的裁定
1. **int 池容量两套口径** ⇒ **两个不同的量**：`1,015,792`（`SYS4INI.BIN` +`0xF0`，安装件）= **存档 pool 块的 int 计数**；
   int 池**容量** = 同头 +`0x114` = **`7,375,580`**（只用于 `VirtualAlloc`/`VirtualLock`）；`3,335,068` 判为**错读 / 来源不明**。
   残留 ⇒ `REQ-01M4GSWZXXFV2Q2Q88QQC68TKB`（来路 + `7,375,580 → 7,375,836` 的 `+256`）。
2. **三个 per-cur dword 格** ⇒ 读点 / 生命周期 / 量纲已查明 · **用途已收口**（只写不读）· **归属仍区分不了**
   （`0x5D8F8 + 4*(30*cur) ≡ 0x5D8F8 + 0x78*cur`）⇒ `REQ-01M4GSW2R999MBJQ9S9EEQ481E`；
   **是否被存档覆盖** ⇒ `REQ-01M48EVKSNX139B8TX0CYXZ0A1`（该单已按纪律收窄 —— 它原先与前者**重复跟踪**了"归属 / 用途"两项）。

## 已登记的遗留
**★ 本轮从"待登记"里划掉（已进台账 + 配会红的守卫与变异条目）**
* ✅ **`local_vars`（脚本头 6 声明）↔ 6 个运行期池逐一对应** —— 6 个连续 u32（文件字节 8/12/16/20/24/28）、
  **每项就是一个 count**（无 type 位 / 槽号位）、声明 ↔ 池 = **位置对应**（第 i 项 = 第 i 个池，type = 9+i）、一次 32 字节定长读、
  6 处展开 store、`mul` 宽度 `4,4,1Ch,4,4,4`、`count=0` 合法。
  台账 `Engine+0x5D880/script-header-local-decls-to-pools`；守卫 `emulator-model.test.mjs#…local 声明…` + `asm.test.mjs#…前 6 个 u32…`；
  变异两条（count 槽对调 / `FIELD_OFFSETS[0]` 8→12）。并 append-only 更正了 `…/pool-allocation-and-capacity` 里
  "按脚本头第 8..13 个 dword 建"那句（那 = 字节 32..52，**读不到**）。
* ✅ **`local_*` 6 池与帧 `+0x1C`/`+0x34` 布局的语料 oracle** —— 计数槽原先只有**字面量自证**，现钉在装载器 6 对「计数源 → 计数槽」store 上。
* ✅ **int 族值 DEC 编码的 key（`Engine[97059]` = `Engine+0x5EC8C`）** —— `(rand()<<16) + rand()`、种子 `timeGetTime()/100`
  （★ `0x51EB851F` + `shr edx,5` 是 **÷100**，不是 ÷32）；全语料**唯一写入点** `0x417359`；`enc_zero = rol32(key,21)`（`sub_405620`）。
  台账 `Engine+0x5EC8C/codec-key-is-rand-derived` + `Engine+0x5EC90/enc-zero-equals-rol-key-21`；
  守卫 `engine-value-codec.test.mjs` 的 G1 / G3；变异一条（`CODEC.keyField.dword` 97059→97060 —— 加它之前**无声通过**）。

**★ 仍未做**
* ⬜ `REQ-01M4GSW2R999MBJQ9S9EEQ481E`（帧记录真实大小 + 三格归属）· `REQ-01M48EVKSNX139B8TX0CYXZ0A1`（三格是否被存档覆盖，依赖存档解码器）
* ⬜ `REQ-01M48FRMKME8VERSH42M7MQMYP`（指令区之后三张表）
* ⬜ **纯数值族**：成员与 handler **已登记**（台账 `opcodes/pure-numeric-family-CORRECTED`，守卫 `emulator-numeric-ops.test.mjs`）——
  未定的只是 `staticUses` 的**走法**（A"文件里有多少条" vs B"引擎会执行多少条"，该记录自己声明"未定"）；
  族内 `0x2D7/0x2D9/0x2DA/0x2DB/0x2DD/0x2DF–0x2E4` **只登记了存在与句柄，语义未解**。
* ⬜ **帧内其它字段**：`frame-fields-FULLANCHOR` / `frame-area-slot-census-CORRECTED` / `…-named-slots` 已登记，
  但**未逐槽**给出"用法形态"的守卫（本轮只补了 `+0x84`/`+0x88` 与帧尾三格）。
* ★ 判据（方向）：每个数据区域有台账条目且锚到可复算观察；模拟器里有能跑的最小模型 + **会红的**守卫；"遇到但未处理"的在树里查得到。
