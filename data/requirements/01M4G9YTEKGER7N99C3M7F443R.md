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

## 要怎么收口

1. 语料来源改成**清单 + ALF 解出**的那一份（与 `emulator-headless-logo.assets.test.mjs` 同源：
   `roots.gameInstall` + `readAlf` 取条目），而不是 `dist/install`；
   ★ 若"散装 `.BIN` + ALF 内条目"的合并集才是那 492 份，就把口径写清（哪一份是"语料"）。
2. 做不到时**明说前提**：把 skip 原因改成"需要先 `pnpm tools release install`（发行树）"，
   并在文件头写清"这条守卫验的是**发行树**不是原始语料" —— ⛔ 不许让"没跑"看起来像"绿"。
3. 判据：把 `0x2D2` 的 `staticUses` 改坏 ⇒ 该守卫必须**当场红**（而不是 skip）。

## 为什么现在只登记不修

它需要**发行树或 ALF 解包能力**二选一，而两者都牵到别的批次（release / age-format 的 ALF 读取）；
本单先把"错在哪、要什么前提"钉住，避免下一个人以为是"守卫写错了"。
