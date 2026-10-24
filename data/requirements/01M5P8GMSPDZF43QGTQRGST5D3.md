# 残差第十七批：负模数 / 按住游标初值 · 网格混合档

- id: REQ-01M5P8GMSPDZF43QGTQRGST5D3
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: tools/test/emulator-new-input-hold.test.mjs#★ 0x104：游标按帧槽号存 —— 帧释放 / 重载后从原值继续（不是 null、不是 0）
- tags: [spec residual]

# 残差第十七批：负模数 / 按住游标初值 · 网格混合档

## 一句话

闭合输入两条残差（负模数 = 向零截断余数；按住游标按帧槽号、第一次 `0x104` 必定义 0），并纠正"网格条目不设混合"的错误。

## 输入

- `0x103/0x104` 的游标推进 = `inc; cdq; idiv; mov [..],edx`，无修正 ⇒ **向零截断余数、符号随被除数**；有定义时游标非负 ⇒ 负模数 ≡ 绝对值（`−7` ≡ `7`、`−1` ⇒ 恒 0）。
- `模数=0` 除零；`游标=INT_MAX ∧ 模数=−1` 商溢出。
- 游标按**帧槽号**存（引擎级数组 + 当前帧号），**释放/重载不复位**；启动 memset 停在模数前，但上下文闸静态格 −1 ⇒ 第一次 `0x104` 必把当前槽写成 0。
- 实现：`Frame.holdCursor` → `Machine.holdCursorBySlot`；溢出诊断 `hold-cursor-divide-overflow`；守卫 +4。

## 场景混合

- `sub_4AEEA0` 是绘制项发射器（`scenario+0x408`，第 12 格 = 混合档，写者 `0x203`）；网格是 `sub_4AF1C0`（`scenario+0x428`，第 9 格 = 混合档，写者 `0x322`）。
- 网格**总会**写混合开再按档写因子；档 0 ⇒ `SRCALPHA/INVSRCALPHA` ⇒ "网格不设混合"错。绘制项档 0 ⇒ 不写因子，档非 0 才写。
- 残差改为 `RES-SCENE-BLEND-TIER-VALUES`（脚本实参分布 / 档 2 守卫）。

## 仍开着

`RES-SCENE-BLEND-TIER-VALUES` · 尾扫链优先级 / 平键序命名待独立复核 · 保存读侧 · 真实绘制 S2+。

## 判据

`pnpm test` 1019/0 · `pnpm test:assets` 203/0（1 skip）· `pnpm test:mutation` 335/335 · `pnpm tools spec validate` 13/13 · `pnpm tools requirements validate` 6/6 · `pnpm typecheck` 六工程绿。

## 怎么查

`pnpm tools spec show CORE-INPUT-103-MODULUS` / `CORE-OPS-INPUT-104` · `docs/02-engine/spec-audit-2026-10.md` §34 · `tools/test/emulator-new-input-hold.test.mjs`。
