# 批 R1 · 引擎通用数据区域（global 池 / local 池 / frame / 纯数值指令）重写核验 + 模拟器建模

- id: REQ-01M48DJ3PK48CXQXAV1M3EEQNR
- type: req
- status: doing
- parent: REQ-01M3TCCVC01AWFEMP6GJM96KQ4
- order: 10
- tags: [engine ledger emulator]

# 批 R1 · 引擎通用数据区域（global 池 / local 池 / frame / 纯数值指令）重写核验 + 模拟器建模

- id: REQ-01M48DJ3PK48CXQXAV1M3EEQNR
- type: req
- status: doing
- parent: REQ-01M3TCCVC01AWFEMP6GJM96KQ4
- order: 10
- tags: [engine, ledger, emulator]

## 范围（★ 只管"引擎体自身存储的通用数据区域"）
1. **global 池** 2. **local 池** 3. **frame 概念** 4. **所有纯数值指令**
5. **迭代系统**：`EA → 指令` 的解析与迭代（够本批用即可）

**明确不含**（用户口径）：❌ 引擎体上基于 offset 的变量；❌ **整体执行流程**；❌ **循环副作用** ——
本批**不处理**，但**遇到就登记**，留后续填补。

## ★ 流程（用户口径：**不许在巨大反汇编文件里漫游**）
```
① context packet（机械、可复现）　② 只解码 + 操作数类型普查（零引擎语义）
③ 帧/池的布局与公式（不取值）　④ 单池取/存（先 int，须含 key/DEC）
⑤ 纯数值指令族　⑥ 迭代系统
★ packet 落 .cache/（派生物）；入库的只有配方与判据
```

## 已完成的点
**① 机械提取器**（`tools/lib/disasm.mjs` + `tools/disasm.mjs`，域 `disasm-at`）
* `EA → 行号 → 有界上下文`；`--cases` 枚举函数里的 switch/跳转表；派生索引落 `.cache/`
* 守卫：`disasm.test.mjs`（4 条 / assets）+ `disasm-extract.test.mjs`（4 条 / pure，合成语料）
* 实测六段合计 530297 行；修 `peOffsetOf` 的 VA/RVA 口径（守卫 `ledger.pe.external.test.mjs`）

**② 操作数类型普查**（零引擎语义）
| 原语 | EA | 跳转表 | case 数 | 入口 | 缺席 |
|---|---|---|---|---|---|
| 取址 | `0x42AEA0` | `jpt_42AF16` | **12** | `sub ecx,3`+`cmp ecx,0Bh` | **无**（case 3..14）|
| 读值 | `0x41BF50` | `jpt_41BF99` | **14** | `cmp edx,0Dh` | **case 8** |
| float 取址 | `0x42B4B0` | `jpt_42B51F` | **10** | — | **cases 5,8,11** |
* 修掉新仓自相矛盾：`getTypeLabel(7)` 原本抛 ⇒ 补 `case 7 → global-float-ptr`
* 新事实：操作数基址 = **帧+0x18**；操作数**每个 8 字节**、opcode 在 `[第一操作数−4]`
* 守卫：`engine-operand-types.test.mjs`（5 条 / assets）

**③ 帧布局（只算布局，不取值）**
* ★ **帧内偏移机械普查**（全语料 530296 行）：帧内共 **50 个槽**，其中 **≥ 0x78 的只有 1 个**（一次 `lea`）
  ⇒ "帧 = 0x78 字节"现在有**四处**独立判据
* ★ **裁决了"帧尾三数组"矛盾**（原 🐞 已收口）：`0x5D8F8/0x5D8FC/0x5D900` 是
  **按 cur 索引、步长 4 的独立 dword 数组**（`[ecx+edx*4+5D8F8h]`，`edx = 2*(15*cur)`，出处 `sub_405640`），
  **不是**帧内字段 —— 旧读数口径（绝对地址直接减 383104）是错的
* 守卫：`engine-frame.test.mjs`（4 条 / assets）
* ★ 顺带修 `project()`：被 `replaces` 指向的记录原先 `effective` **没变成 `retracted`**（只在冲突分组里被排除）

## 台账（7 条，锚全部可解析）
accepted 5：dispatch[0x400] 可静态再提取（544 条与旧仓逐条一致）·
三原语支持集 · 帧步长 0x78 · 三数组为 cur 索引 · 帧区槽位普查
proposed 1：int 池容量两套口径（1,015,792 vs 3,335,068）
retracted 1：原"帧尾三数组"读法（已由更正记录取代，历史保留）

## 已登记的遗留
* 🐞 `REQ-01M48E8HHKYM854RXN9ZG8DSH7`（**已 done**）：矛盾已裁决，见上
* ⬜ 新开：那三个 cur 索引数组的**用途**未查明（`lea ebx,[esi+5D8F8h]` + `mov edi,3` 提示"基址 + 3 项"）
* 待登记：`local_*` 6 池与帧 +0x1C/+0x34 布局 / 帧内其它字段 / int 族值 DEC 编码（key = `Engine[97059]`）/
  **纯数值指令 28 条**（0x50–0x5F / 0x60 / 0x135/0x136/0x13F / 0x191 / 0x2D0–0x2D6）
* 缺口：`local_vars`(脚本头 6 声明) ↔ 6 个运行期池逐一对应；0x2D7 / 0x2D9 / 0x2DF–0x2E4 语义未解

## 判据（待写实；方向）
① 每个数据区域有台账条目且锚到可复算观察；② `apps/emulator` 里有能跑的最小模型 + **会红的**守卫；
③ "遇到但未处理"的东西在树里查得到。
