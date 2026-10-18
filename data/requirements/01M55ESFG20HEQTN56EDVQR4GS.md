# 批 S2h · 内嵌子对象 size（12 个有独立证据）+ "父访问落在子对象里"的归属判据

- id: REQ-01M55ESFG20HEQTN56EDVQR4GS
- type: req
- status: done
- parent: REQ-01M54KEF7X560CMPMGABG2W3CW
- verify: tools/test/shape-claims-guards.test.mjs#★⑳
- tags: [engine shape]

# S2h · 内嵌子对象的 `size` + "父访问落在子对象里"的归属判据

`T_Command` 的 20 个内嵌子对象原来全是 `size:null` ⇒ G8 明说"元素 size 未知 ⇒ 跨度算不出、宿主匹配按 **1 字节** 槽静默退化"。
本单把能证的 `size` 补上，并把由此暴露的两条**归属**判据落进工具。

## 证据（12 个 size，三个档位）

| 档位 | 类型 |
|---|---|
| **分配点**（独立证据） | `T_Stack_int`=0x14（`new(0x14)` 后内联构造）· `T_Queue_int`=0x1c（`new(0x1c)` @0x4221b4/0x40e337）· `T_offaab74`=0x1c |
| **lo==hi 坍缩**（写入上界正好顶到下一个已证对象起点） | `T_Sound`=0x834 · `T_Data`=0x4bc · `T_Hook`=0x414 · `T_SE`=0x4e4 · `T_Font`=0x3967c · `T_ReadTextDB`=0x478 · `T_FileDB`=0x4048 · `T_SysCursor`=0x430 |
| **eh vector 立即数** | `T_FadeTimer`=0x1c（32 条，跨度 0x380）· `T_offaab74`=0x1c（3 条，跨度 0x54 = `0xaabc8−0xaab74`，正好收尾） |

布局自洽（相邻 K 差 == size）：`0x48e8+0x834=0x511c` · `0x511c+0x4bc=0x55d8` · `0x13fa8+0x414=0x143bc` ·
`0x143bc+0x4e4=0x148a0` · `0x14d30+0x3967c=0x4e3ac` · `0x4e3ac+0x478=0x4e824` · `0xa609c+0x4048=0xaa0e4` ·
`0xaa0e4+0x430=0xaa514`。

**仍只有区间（不写 `size`）**：`T_Input`[0x1adc,0x1ae0] · `T_Sys3Draw`[0x29f8,0x2a00] · `T_Voice`[0x484,0x490] ·
`T_Renderer`[0x470,0x4ec] · `T_Scene`[0xda2c,0xda30] · `T_Hashtable_int`[0x50,0xe6e4] · `T_Queue_CACHE_ALPHADATA`[0x1c,0x20] ·
`T_Reg`[0xc0,0xc4] · `T_Music`[0x440,0x59c]。

## 两处**归属**订正（提案点名 + 我按 EA 复核）

| 订正 | 依据 |
|---|---|
| `T_Command+0xaa514`：`kind:"ptr" type:"T_Reg"` → **内嵌对象** | `0x415917 lea ecx,[esi+0AA514h]` + `call sub_491880`（就地构造） |
| `T_Command+0x511c`：`width:4` → **内嵌对象** | `lea ecx,[esi+511Ch]` + `call sub_4388C0` |
| `T_FadeTimer` 实例 `count:1` → **32**；`T_Queue_int` 实例 `count:1` → **3** | eh vector 的条数/元素宽立即数 |
| `T_offaab74` → `T_Queue_int`（RTTI `??_7?$Queue@H@@6B@`）；`T_off6927c` → `T_Hashtable_int` | vtable 名 |

## 两条判据（并加了守卫 ★⑳ + 两条变异）

1. **父访问落在内嵌子对象跨度里** ⇒ 归属是子对象的，但**站点仍要解释**（去子对象的成员表里找）；找不到才记残差，
   且残差要点名**子对象类型**（否则看不出该补谁）。
2. **元素 0 的偏移 0 = 数组/内嵌对象自己的起点** ⇒ 由成员本身解释（`lea ecx,[esi+K]` 不该变成"元素内偏移 0x0 不是成员"）。

★ 效果：`c92`（构造器载体）**889 站 = 解释 879 + 残差 11**（合并前是 873/17）；G7 空话声明 24 条。

## 诚实项

- `lo` 里含**递归进被调方**的写（只有 `T_Data` 依赖这条：0x4 → 0x4bc）；`loDirect` 逐条在 `evidence.txt`。
- 助手在 `sub_454A20`（FileDB 构造器）上给 **0 站点**，`T_FileDB` 的 0x4048 只能逐字读 `.lst`（循环 `[eax+404h]` 末次）。
- 容器构造器在同段的写**未采纳**进 size（只作线索）。
- `T_Command+0x796a0` 仍是"提案 scalar vs G5' 要求 ptr"的冲突（进 undetermined，未写声明）。
- `T_Command+0x5d6e0` 与 `T_Command+0x796a0` 的 4 处站点仍是残差（表/冲突未裁决）。

## 复现

`pnpm tools shape claims data/shape/carriers.json`（c92 889/879/11 · `T_Command` 972 成员）· `pnpm test`（397）·
`pnpm tools mutate check --match 元素 0 的偏移 0`。
