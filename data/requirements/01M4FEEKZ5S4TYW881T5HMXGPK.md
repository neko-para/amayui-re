# 🐞S2 local 池基址：语料逐字 = 帧+0x34..+0x48（读侧+装载器+ENC(key,0) 三者互证），代码/守卫写 0x3C..+0x54 且守卫绿而错

- id: REQ-01M4FEEKZ5S4TYW881T5HMXGPK
- type: bug
- status: done
- parent: REQ-01M4AGPP1T9A60HQYW3GX9W592
- verify: tools/test/emulator-model.test.mjs#★ local 池的 slot 几何：**6 个整齐基址**（与旧仓一致）—— 且按 EA 回语料核过（装载器 6 处 store + 6 个 type 的读侧 case 双向对上）
- repro: tools/test/emulator-model.test.mjs#★ local 池的 slot 几何
- severity: S2
- done_reason: 按用户口径"冲突以最新复核结论为准"裁定为语料侧（帧+0x34/0x38/0x3C/0x40/0x44/0x48）：两条独立判据（装载器"读计数→new[]→写基址"的 6 处 store；取址原语 case 号=operand type 的配对）+ 一条旁证（ENC(key,0) 填在 +0x34 那池）。已改 LOCAL_POOL_SLOTS、把守卫判据从"偏移在语料里出现过"（绿而错）换成"双向对上"、加两条变异，并按 append-only 追加台账更正记录。
- tags: [emulator, layout, guard]

# 🐞S2 local 池基址：语料逐字 = 帧+0x34..+0x48（读侧+装载器+ENC(key,0) 三者互证），代码/守卫写 0x3C..+0x54 且守卫绿而错

- id: REQ-01M4FEEKZ5S4TYW881T5HMXGPK
- type: bug
- status: open
- parent: REQ-01M4AGPP1T9A60HQYW3GX9W592
- repro: tools/test/emulator-model.test.mjs#★ local 池的 slot 几何
- severity: S2
- tags: [emulator, layout, guard]

## 结论（本轮独立复核，两条独立取证线）

local 池的 **6 个基址偏移**与 **type 9..14 的配对**，语料逐字支持的是 **`帧+0x34 … 帧+0x48`**；
而本仓 `packages/age-format/src/engine/layout.mts` 的 `LOCAL_POOL_SLOTS` 与守卫
`tools/test/emulator-model.test.mjs` 现在写的是 **`0x3C … 0x54`** —— 后者**与二进制不符**。

| 池 | type | 读侧逐字（`sub_42AEA0`/`sub_41BF50` 的 case） | 装载器写点 | 现在代码/守卫写的 |
|---|---|---|---|---|
| int | 9 | `.lst:66266 [esi+edx*8+5D8B4h]` = 帧+**0x34** | `.lst:25262 .text:0040F2E9 [esi+edx*8+5D8B4h]`（紧随 `[5D89Ch]` 的 `inc` ⇒ count=帧+0x1C） | 0x40 |
| float | 10 | `.lst:66294` 折叠形 = 帧+**0x38**（`.text:0040F321 add ecx,0C79h` ⇒ `120*0xC79 = 0x5D8B8`） | `.lst:25282` | 0x44 |
| string | 11 | `.lst:66314 / 41634 [esi+edx*8+5D8BCh]` = 帧+**0x3C** | `.lst:25332 .text:0040F3B1` | 0x3c ✅（唯一一致的一格） |
| ptr | 12 | case 12 ⇒ 帧+**0x40**（`5D8C0h`） | `.lst:25351` | 0x48 |
| floatPtr | 13 | 帧+**0x44**（`5D8C4h`） | `.lst:25370` | 0x50 |
| stringPtr | 14 | `sub_41B640` case 14（`.lst:40901 [ecx+5D8C8h]`）= 帧+**0x48** | `.lst:25390` | 0x54 |

**第三者确证**（让"帧+0x34 那一池是 int"不再是推断）：`.lst:25404 mov eax,[eax+5D8B4h]` +
`.lst:25407 mov [eax+ecx*4],edx`，而 `edx = [esi+5EC90h] = ENC(key,0)`（int 池初值口径）。

## 为什么现在**不**改（口径，不是懒惰）

`REQ-01M4B49MK0HMJ7NK00PN4SMDHB`（🐞S3，**已 done**）的 `done_reason` 记的是**用户口径裁决**：
「以本仓重新发现的内容为准」⇒ 基址改为 `0x3C/0x40/0x44/0x48/0x50/0x54`。
本轮的取证**指向相反**，而且指出那条"新取证"的表**自相矛盾**：它引的 `0x50/0x54` 那两个偏移
在语料里是**别的帧字段**（`frameArg` / `triples[0].len`），不是池基址。
⇒ `AGENTS.md` §6.4：冲突**显式化**为产物、**不许**静默改写已有结论。所以本单只摆证据，
**请裁决**；裁定后按"追加更正记录（`replaces`）"落地。

## ★ 更要紧的一点：那条守卫是**绿而错**

`node --test --test-isolation=none tools/test/emulator-model.test.mjs` = **12 pass / 0 fail**，
但它**只要求这些偏移"在语料里出现过"** —— 而 `0x50(5D8D0h)` / `0x54(5D8D4h)` 作为别的字段确实出现
⇒ 蒙混过关。它同时断言 `int.count = 0x1C` 与 `int.base = 0x40`，而语料里 count 在 `帧+0x1C` 的那个池
其 base 写在 `帧+0x34` ⇒ **守卫与它自己引用的语料自相矛盾**。

## 收口判据（三条都要）

1. `LOCAL_POOL_SLOTS` 的 6 个 base 按**裁决**改（或确认现表对），并把 `FRAME_LAYOUT.off.base0 = 0x34`
   与它**对齐**（同一份 `layout.mts` 里这两处今天就自相矛盾）。
2. **守卫改判据**：从"偏移在语料里出现过"改成"**按 EA 回语料核 store 目标**"
   （`sub_40ED40` 的 6 处 store 与 6 个 type 的读侧 case 双向对上）；
   `tools/test/emulator-model.test.mjs:120/137/156` 的三处硬编码同时改。
   ★ 判据：改错一个偏移，这条守卫必须**当场红**（现在是绿的）。
3. 台账：把本条证据**追加**成一条更正记录（`replaces` 指向被更正的那条），
   并把本轮已写的对照 note（`Engine+0x5D880/local-pool-bases/code-vs-corpus`）链上。

## ✅ 已收口（2026-10，按用户口径"冲突以最新复核结论为准"）

**裁定：语料侧胜** —— 基址 = `帧+0x34/0x38/0x3C/0x40/0x44/0x48`（与旧仓 `fields.json` 一致，
但**不是因为旧仓写了它**，而是因为它被两条独立判据复核过）：

1. **装载器 `sub_40ED40`**：形态是"读计数 → `operator new[]` → 写基址"，6 处 store 逐字留在
   `layout.mts` 的 `LOCAL_POOL_SLOTS` 头注里（`0x40F2E9` 那条 = 帧+0x34 —— 上一版读成了 +0x40）；
2. **取址原语 `sub_42AEA0` 的跳转表 case 号 = operand type**（case 9→+0x34 … case 14→+0x48）；
3. **旁证**：`ENC(key,0)` 初值填在 `帧+0x34` 那一池（`.lst:25404/25407`）⇒ 那格是 int。

**改了什么**：`LOCAL_POOL_SLOTS` 六值 + 守卫**换判据**（"偏移出现过" → "装载器 store 与读侧 case 双向对上"；
旧判据**绿而错**：`+0x50/+0x54` 是别的帧字段）+ 两条变异；台账 `…/RESOLVED` 记裁决与依据。
