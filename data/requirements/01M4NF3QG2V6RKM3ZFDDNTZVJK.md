# 存量欠账：在场 reference bin 锚的 len+sha256 逐条补齐（棘轮只许下降）

- id: REQ-01M4NF3QG2V6RKM3ZFDDNTZVJK
- type: req
- status: open
- parent: REQ-01M3TD4N309J3Y6YMWWR2GTQAA
- order: 130

## 现状（可复算）
`pnpm tools ledger report` 的"摘要欠账"那一行 = **276 条在场 `reference` `bin` 锚缺 `len`+`sha256`**
（2026-10-11 由 **338 → 282 → 276**）。棘轮上限 `REFERENCE_BIN_DIGEST_DEBT_MAX`（`tools/lib/ledger.mjs`）
与现算值**恒相等、余量恒 0** ⇒ 再多一条就红（口径与 `coverage` 的棘轮同形：存量钉成上限、只许下降）。

★ 新记录已被**两处**管住，本单只管**存量**：
* **写路径**：`ledger add` 在落盘前逐条硬拒（`len` 或 `sha256` 缺一即拒，dry-run 也拒）；
* **门禁**：不变量 #8 的棘轮（现算值 > 上限 ⇒ 红），守卫 `tools/test/ledger-reference-bin-digest.test.mjs`。

## 怎么还（两条路，都要留痕）
1. **被触碰时补**（默认处置；2026-10-11 已用 12 次）：一条记录因为别的理由要追加"订正版"
   （锚点重挂 / 站点订正 / 参考件路径订正）时，顺手把它的每个 `reference` `bin` 锚补 `len` + `sha256`。
   `len` = 该 EA 处那条指令的字节数（函数起点取整个函数体；**折叠站点**取到 listing 的下一条位置）；
   `sha256` 一律由 `node tools/ledger.mjs --hash-anchor <path> <ea> <len>` **现算**，不许手抄。
   那一轮 12 条订正版一次消掉 62 条欠账（338 → 276）。
2. **另批机械回填**（**未做**，要单独一轮）：对未被触碰的存量逐条补，判据同上。
   ★ **顺序硬要求**：先过站点口径（`REQ-01M4N3QC4SYG4C36QDBQCFRMSW`），再补摘要 ——
   否则 `sha256` 会把"锚在指令中间 / 锚在与 claim 无关的函数里"的错锚一并**祝福**成"已核验"，
   比缺摘要更难发现。

## 判据（收口时挂）
* 每补一批 ⇒ `report` 的"摘要欠账"下降 ⇒ **同一次改动里**把 `REFERENCE_BIN_DIGEST_DEBT_MAX`
  调到新实测值（余量恒 0，`report` 会打出余量）；
* 存量欠账归零那天可 `done`：`verify: tools/test/ledger-reference-bin-digest.test.mjs#★ 摘要棘轮：新记录缺 len/sha256 ⇒ 红；存量同样缺 ⇒ 不红（双向）`；
* 每次回填都能指出"这个 `sha256` 是 `--hash-anchor` 现算的"（不手抄 = 可复核）。

## 不在本单范围
* **站点口径**（`bin` 锚是否只许落 claim 逐字引用的 `.lst` 站点；"刻意的字节锚"算不算合法）
  → 缺陷单 `REQ-01M4N3QC4SYG4C36QDBQCFRMSW`（含本轮 25 个"待裁决"锚的逐条清单）；
* PE 映射本身与"`--add`/`validate` 是不是同一个数" → 已由
  `tools/test/ledger.pe.external.test.mjs` + `tools/test/ledger.test.mjs` + 一条变异钉住。
