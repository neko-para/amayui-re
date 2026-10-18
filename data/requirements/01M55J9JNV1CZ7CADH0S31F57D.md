# 批 S2l · T_FileDB 成员表（22 成员 · 末端 0x4048=size）+ 4 处既有成员订正 + 1 处拒改（G4 末端零引用）

- id: REQ-01M55J9JNV1CZ7CADH0S31F57D
- type: req
- status: done
- parent: REQ-01M54KEF7X560CMPMGABG2W3CW
- done_reason: 数据类合并（22 成员 + 4 处订正 + 1 处按 G4 拒绝）；由 claims 全绿、守恒①/②/③ 与 calls 0 不一致背书
- tags: [engine shape]

# S2l · `T_FileDB` 成员表（22 成员 · 无缝咬合）+ 4 处既有成员订正 + 1 处**拒改**（G4 不许）

`T_FileDB`（RTTI `??_7FileDB@@6B@`，构造器 `sub_454A20`，vtable 写入 `0x454a64`，`size` = **0x4048**）
是 `Command+0xa609c` 的内嵌子对象，紧接在 `T_Command+0xa509c` 那张 **1024 项分派表**之后（表尾正好 `0xa609c`）。

## 合并了什么

- **新增 11 条**：`T_FileDB` 10 条（`0x408` `0x418`(ptr) `0x420` `0x2c28`(×256) `0x3430` `0x3434`(ptr) `0x343c`
  `0x3440`(×256) `0x3840`(ptr) `0x3c44`(ptr)）+ `T_FileDB_424+0x10` → 新类型 `T_FileDB_424_10`（24 字节 =
  步长 `0x28`−`0x10`；`Initialize/DeleteCriticalSection` 的实参，witness `0x454ac3`/`0x4553e8`）。
- 全表 **22 条**按 off 无缝咬合，末端 `0x3C48+0x400 = 0x4048 = size`。

## 我作为合并方改了 4 处**既有成员**（报告 §A6 点名、子代理按"补丁只写新成员"的口径没动）

| 现声明 | 改为 | 证据 |
|---|---|---|
| `0x3028 {width:4}` | **`T_el32 × 256`** | `0x454ad6 lea edi,[esi+3028h]` + `0x454adc mov ecx,100h` + `0x454ae6 rep stosd`；索引 `0x454fb2 mov ecx,[ecx+edx*4+3028h]` |
| `0x3844 {width:4,kind:ptr}` | **`T_el32 × 256`** | 平行循环按 4 字节清 `0x100` 次；索引 `0x454986 mov ecx,[edi+ebx*4+3844h]` |
| `0x410 {width:4}` | **`kind:"ptr", type:null`** | `0x454d53` 判非空 → `0x454d5c/0x454d64` `GlobalAlloc` 返回值写回 |
| `0x3c48 {kind:ptr}` | **拒绝数组化，保持单槽 + note** | 报告认为它也是 256 格数组（`0x454999 cmp dword ptr [edi+ebx*4+3C48h],0`），但**末端 `0x4048` 在语料里零引用** ⇒ G4 判"`count` 无依据"。⛔ 不为变绿硬给 count；数组化待裁决（见 `T_FileDB+0x3c48` 的 `note`） |

★ 前 3 处订正后 `0x3028` 那 1 处残差消掉（`sub_455000@0x455041` 的 `lea edi,[esi+302Ch]` = 该数组第 2 个 dword）。

## 任务 B：`sub_4BB810` 的 `T_SE`/`T_Voice` 冲突 —— **成因有 EA 判据，属主明确判不了**

- 体：只写 `[ecx+408h]/[ecx+40Ch]/[ecx+410h]`（`0x4bb819/22/28`），**不写 vtable**、无分支。
- 两个调用方：`lea ecx,[esi+143BCh]`（= `T_SE`，`T_Command+0x143bc`）与 `[esi+148A0h]`（= `T_Voice`）；
  `0x148A0 − 0x143BC = 0x4E4` **正好 = `T_SE.size`**。
- `SE::SE`(`0x4b4e93`) 与 `Voice::Voice`(`0x4bb733`) 以**相同顺序与值**初始化同一三元组、都置 `+0x414=1` ⇒ **布局相容**。
- 但**属主判不了**：该前缀不在 `T_SE` 表里（它从 `0x418` 起）、不在空的 `T_Voice` 表里、不在只证过 `0x0/0x4` 的
  `T_BaseObject` 里；缺"被两类共同调用的中间基类构造器或 RTTI 名"，且三元组是在各自 vtable 写入**之后**才初始化。
  ⇒ `carriers-patch` 留空（⛔ 没硬加载体）。

## 诚实项

- `pnpm tools shape access sub_454A20` 默认给 **35 处全部 `base=unresolved`**（`esi` 在 `0x454a5a` 的 `call` 处被判死；
  `--calibrated` 下 25 处解成 `this`）—— 构造器结论**全来自逐字读 `.lst`**，不是靠这个工具的默认口径。
- 新加的 4 条 `kind:"ptr"` 进了 **G7 空话声明**（解引用都在还没载体的函数里）⇒ G7 从 44 涨到 **48**，⛔ 不算通过。
- `0x3c48` 的数组化被 G4 挡住（末端零引用）—— 这与"奇数跨度末尾对齐"那条 note 是同一类现象：**oracle 看不见的边界**。

## 复现

`pnpm tools shape claims data/shape/carriers.json`（守恒① 5130 = 4774 + 356 · 全绿）· `pnpm test`（398）·
`node .tmp/shape/batch-09/filedb/build.mjs --check`（逐字节绿）。
