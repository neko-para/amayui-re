# 模拟器纯数值族的 corpus 来源：`<repo>/dist/install`（发行产物 + 会被 patch）应换成清单 gameInstall 解出的原始语料

- id: REQ-01M4G9YTEKGER7N99C3M7F443R
- type: req
- status: done
- parent: REQ-01M4FEF7VH1N0JDT5ZVPZS204M
- verify: tools/test/emulator-numeric-ops.test.mjs#★ 静态出现次数可复算：0 次的那批必须真的是 0（不是"没数到"）
- tags: [emulator, guard, assets]

# 模拟器纯数值族的 corpus 来源：`<repo>/dist/install`（发行产物 + 会被 patch）应换成清单 gameInstall 解出的原始语料

- id: REQ-01M4G9YTEKGER7N99C3M7F443R
- type: req
- status: open
- parent: REQ-01M4FEF7VH1N0JDT5ZVPZS204M
- tags: [emulator, guard, assets]

## 现场

`tools/test/emulator-numeric-ops.test.mjs` 的两条 corpus 用例要 `.BIN` 语料，来源是
`path.join(REPO_ROOT, 'dist', 'install')` —— 那是 **`pnpm tools release install` 的发行产物**：
* 本机没有它 ⇒ 两条用例 **skip**（`node --test` 退出码 0）⇒ `pnpm test:mutation` 里那条变异
  "看起来没被抓住"，其实是**没跑**（已把 `tools/mutate-check.mjs` 改成能分辨 skip：`⏭ 无法判定`）；
* 更要紧：`dist/install` 会**应用补丁** ⇒ 用"发行树"当"语料现算"的基准，等于让
  `numeric-ops.ts` 里的 `staticUses` 与"这一台机器上的发行树"对齐，**换一台机器就可能不一致**；
  而 `staticUses` 当初是照**原始语料**（492 份反汇编 `.BIN`）数的。

## 已落地（2026-10，本轮）

1. 语料来源改成**清单 + ALF 解出**的那一份（与 `emulator-headless-logo.assets.test.mjs` 同源）：
   `rootFromAssetsJson(REPO_ROOT, 'gameInstall')` → `SYS4INI.BIN` 当索引 → `readAlf` + `loadPayloads`
   → **565 个 `.BIN` 条目**（本机实测 565 个全部取到）。★ 口径：**ALF 索引里的 `.BIN` 条目**
   （散装 `.BIN` 不入这份语料 —— 它不是"清单指定的原始来源"）。
2. 语料是 `external-only` 素材 ⇒ 缺席时仍然 `skip`，但 skip 原因已写清是**哪一步缺**
   （清单没有根 / ALF 索引不在 / ALF 取不全），⛔ 不再是"没跑看起来像绿"。
3. `staticUses` 全部 39 条按新语料**重数了一遍**（旧值 = 发行树那一份）：两源**不是同一份语料** ——
   实测 `0x50` 原始 62945 / 发行树 70439；**零出现集合 6 条 → 7 条**（`0x2E4` 进来）。
   逐条口径与"两种走法"的分叉都写在 `apps/emulator/src/model/numeric-ops.ts` 的文件头。
   ★ 这一条**已追加台账 note**（subject `model/numeric-ops-staticuses-corpus-source`，`KN-01M4GXT5ES31425Y4F0Q652C3T`）。
4. 判据：把 `0x2D2` 的 `staticUses` 改坏 ⇒ 该守卫**当场红**（本机实测 `fail 1`；`pnpm test:mutation`
   里那两条 `0x2D2` 变异也不再是"无法判定"）。

## 判据（收口）

`verify: tools/test/emulator-numeric-ops.test.mjs#★ 静态出现次数可复算：0 次的那批必须真的是 0（不是"没数到"）`
—— 它在本机**真的跑**（`pass 1 / skipped 0`），且两次 `0x2D2` 变异都会让它红。
