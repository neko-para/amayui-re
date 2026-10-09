# 🐞S3 冲突 · local 池基址：两次「新取证」互相矛盾（最终以「装载器 store + 取址原语 case 号」双向核对为准，回到 +0x34…+0x48）

- id: REQ-01M4B49MK0HMJ7NK00PN4SMDHB
- type: bug
- status: done
- parent: REQ-01M4B248NB8FWDN61E2NMK8N6V
- verify: tools/test/emulator-model.test.mjs#★ local 池的 slot 几何：**6 个整齐基址**（与旧仓一致）—— 且按 EA 回语料核过（装载器 6 处 store + 6 个 type 的读侧 case 双向对上）
- repro: tools/test/emulator-model.test.mjs#★ local 池的 slot 几何：**6 个整齐基址**（与旧仓一致）—— 且按 EA 回语料核过（装载器 6 处 store + 6 个 type 的读侧 case 双向对上）
- severity: S3
- done_reason: 已收口，但**结论翻过两次**：第一次"以本仓重发现为准"改到 0x3C…0x54（错，根因是把 0x40F2E9 的 store 读成 帧+0x40）；第二次复核按用户口径「冲突以最新复核结论为准」回到 0x34…0x38…0x48 —— 判据是"装载器读计数→new[]→写基址"的相邻性 **加** 取址原语 case 号（=operand type）的配对，两条一起把六对配光。同时把守卫从"偏移在语料里出现过"（绿而错，+0x50/+0x54 是别的帧字段）改成"两件能定配对的事"，并加两条变异钉住。

## 这条冲突（已两次翻案，最终以**双向核对**收口）

**同一件事有三份说法**（local 池的基址）：

| 来源 | 基址（按池名 int/float/string/ptr/floatPtr/stringPtr） |
|---|---|
| 旧仓 `fields.json`（**最终被证实是对的**） | `+0x34 / +0x38 / +0x3C / +0x40 / +0x44 / +0x48` |
| 本仓第一次"新取证"（2026-10 早，**错**） | `+0x40 / +0x44 / +0x3C / +0x48 / +0x50 / +0x54` |
| 本仓第二次复核（2026-10 晚，**采用**） | `+0x34 / +0x38 / +0x3C / +0x40 / +0x44 / +0x48` |

## 第一次为什么错（可复核的根因）

不是"旧仓对/本仓错"，而是**把装载器的 store 地址抄成了别的偏移**：
它引用 `0x40F2E9` 说"写 `帧+0x40`"，而逐字是
`25262 .text:0040F2E9 mov [esi+edx*8+5D8B4h], eax` ⇒ `0x5D8B4 − 0x5D880 = 0x34`。

## 收口判据（两条独立判据 + 一条旁证，**不再靠"分配顺序"猜配对**）

1. **装载器 `sub_40ED40`**：每一步是"读计数槽 → `operator new[]` → 写基址槽"，
   6 处 store 逐字见 `packages/age-format/src/engine/layout.mts` 的 `LOCAL_POOL_SLOTS` 头注；
2. **取址原语 `sub_42AEA0` 的跳转表 case 号 = operand type**：
   case 9→`+0x34`(int) · 10→`+0x38`(float) · 11→`+0x3C`(string) · 12→`+0x40`(ptr) · 13→`+0x44`(floatPtr) · 14→`+0x48`(stringPtr)
   （`.lst:66246/66256/66278/66288/66313/66325` 的 case 标签 + 各分支的池读取）；
3. **旁证**：`ENC(key,0)` 的初值填在 `帧+0x34` 那一池（`.lst:25404/25407`，`edx = [esi+5EC90h]`）⇒ 那格是 int 池。

## ★ 更值钱的产物：**守卫从"绿而错"改成"会红"**

旧守卫问的是"这个偏移在语料里**出现过**吗" —— 而 `+0x50`/`+0x54` 作为**别的帧字段**确实出现过
⇒ **错表照样通过**（实测 12 pass / 0 fail）。现在改成问两件能定配对的事：
① 装载器分配点写下的地址集合；② 6 个 type 的 case 各自读哪个地址。
⇒ 改错一个偏移、或把两个池对调，都会当场红（`tools/mutate-check.mjs` 里有两条对应变异）。

## 落地

* `packages/age-format/src/engine/layout.mts` 的 `LOCAL_POOL_SLOTS` → 上述六值（逐字与两次订正的经过写在该常量头注里）；
* 守卫用例改名并换判据：`tools/test/emulator-model.test.mjs` 的「★ local 池的 slot 几何：**6 个整齐基址**（与旧仓一致）—— 且按 EA 回语料核过（装载器 6 处 store + 6 个 type 的读侧 case 双向对上）」；
* 台账按 append-only 追加更正记录：`sub_414AC0|sub_40ED40/pool-allocation-and-capacity` 与
  `Engine+0x5D880/local-pool-bases/code-vs-corpus/RESOLVED`（后者记的是**裁决与依据**）。
