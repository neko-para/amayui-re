# 批 S2m · 一个明确判不了的挂起项：T_FileDB+0x3c48 的数组化（G4 末端零引用）

- id: REQ-01M55JBME4W49EPGPH8W9XZP40
- type: req
- status: done
- parent: REQ-01M54KEF7X560CMPMGABG2W3CW
- done_reason: G4 加第三档（end === size）后 T_FileDB+0x3c48 升成 T_el32×256；条数证据 = 索引取顶字节 sar edi,18h（[0,256)）+ 末尾 0x4048 正好是 size；守卫 ★㉔ + 两条变异
- tags: [engine shape]

# S2m · `T_FileDB+0x3c48` 的数组化（G4 的 oracle 看不见它）—— **已解**

> 原本还有第 ① 条 `sub_4BB810` 的属主 —— **已由 S2o 定案**：它是**布局适配器**（`T_408_trio`，`adapter:true`），
> 依据 = RTTI 基类数组 `??_R2SE@@8`/`??_R2Voice@@8` 条目数都是 2（自己 + `BaseObject`），而校准
> `??_R2Sound@@8` 是 3（含 `SoundError`）⇒ 中间基类**会**出现在数组里，SE/Voice 没有共同中间基类。
> 传播冲突因此 1 → 0。

## ② `T_FileDB+0x3c48`：已升成 `T_el32 × 256`

原来卡在"数组末端 `0x3c48 + 0x400 = 0x4048` 在语料里零引用 ⇒ G4 拒给 `count`"。**这其实不是"没有证据"，
而是 G4 的 oracle 少了一档**：

1. **条数的独立证据本来就有**：索引是**顶字节** —— `0x404A86 mov edi,ebx` + `0x404A88 sar edi,18h`
   （随后 `0x404A92 and ebx,0FFFFFFh` 取低 24 位当元素内下标）⇒ 索引域 `[0,256)`，
   与 `0x3844`/`0x3428` 那几张表**同一把索引、同一步长 4**。
2. **末地址零引用是必然**：`0x4048` 正好是 `T_FileDB.size` —— 数组**正好填满对象**，
   末地址落在对象之外，编译器不会把它的绝对地址算出来。

⇒ 给 G4 加**第三档**：`end === size(容器)` ⇒ note"数组正好填满对象"（⛔ 不是"末尾有引用"那一档）。
它不是放行，等式可证伪：`size` 大 4 而末尾不贴边 ⇒ 回落到红；`count` 写大一格 ⇒ 越过 `size`，由 G1 当场红。
两处都有用例（`★㉔`）与两条变异（拆掉第三档 / 把 `===` 放宽成 `<=`）。

## 落点

- `data/shape/types.json`：`T_FileDB+0x3c48 → {type:"T_el32", count:256}`（成员 22 → 22，形态由单槽变数组）
- `tools/lib/shape-claims.mjs`：G4 第三档
- `tools/test/shape-claims-guards.test.mjs`：`★㉔`
- `tools/mutate-check.mjs`：两条（第三档被拆 / 等式被放宽）
- `docs/02-engine/shape-census.md`：「第三档：数组正好填满对象（末尾 == `size`）」

## 复现

`pnpm tools shape claims data/shape/carriers.json` ⇒ G4 对 `T_FileDB+0x3c48` 现在打印"末尾 0x4048 == 容器 size"；
`node --test --test-isolation=none tools/test/shape-claims-guards.test.mjs` ⇒ `★㉔` 绿。
