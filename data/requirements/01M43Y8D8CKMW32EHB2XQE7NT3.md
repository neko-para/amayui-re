# APPEND 追加包 ヘタレ 词/名分流的命名复核（ヘタレス 系专名 vs 废柴 / 弱者的语义）

- id: REQ-01M43Y8D8CKMW32EHB2XQE7NT3
- type: req
- status: open
- parent: REQ-01M3TCENZ0BCBBFEEDJNMDR75K
- tags: [translation patch]

## 为什么提出来（用户口径）
「弱者的遗迹」周边的相关内容整体走 **赫塔XXX**（音译）路线，原词根 `ヘタレ`（废柴 / 弱者）的语义在专名里丢了：
日文侧 `ヘタレ`（普通词）与 `ヘタレス`（专名）**同根**，中文侧却是两个互不相干的词。要求：评估能否优化。

## 现状（机械清点，可复跑）
```bash
pnpm tools patch find --regex 'ヘタレ|へタレ|ヘタテット|ヘタボウ' --count
pnpm tools patch find --regex 'ヘタレ|へタレ|ヘタテット|ヘタボウ'      # 日文 ↔ 中文 配对明细
pnpm tools patch find --regex '赫塔|废柴' --count
```
两支口径：普通词 = **废柴**（`$1$ITINIT` 攻击名 · `$1$PLINIT` 的 `ヘタレの証` · `$1$SC1820` 难度选项 ·
`$1$SG1822C` · `$1$SG6030` · `$1$SG6832A` · `INFOFA`）；
专名 = **赫塔X**（`ヘタレス`→赫塔雷斯 · `ヘタテット`→赫塔泰特 · `ヘタボウ`→赫塔宝 · `ヘタレン`→赫塔连 · `ヘタレ種`→赫塔种）。
★ 两种写法在**同一个 UI 列表里并排**（`$1$PLINIT` 264/265：【设施：赫塔泰特的像】／废柴的证明）。

## 决策史（`ref/archive.zip`，只读快照：只回答"当时提过什么"，结论与计数一律不采信）
* `archive/prob/prob-SG追加包.md` §2：**词 / 名分流**（普通词 废柴；同前缀专名保持音译根，不随词改「废柴」）；
* `archive/prob/prob-追加包系统.md` §1：2026-08-12 **方案 A 定稿**"专名保持赫塔X"，并注"待 `$1$SC6830` 剧情定稿"；
* `ref/assets/keywords/keywords-战斗地名.md` / `keywords-装备与物品.md` 至今仍写 `ヘタレス 待定（谐音 ヘタレ）`
  —— ★ 文档侧与既成译文**不一致**；快照是只读的（守卫钉字节），不要回写它。

## 判据（收口）
1. **用户裁决**（字母候选 + 主推由会话给出；**未裁决前一个字都不许动**）；
2. 裁决后回改走"生成清单 + **逐条确认** + 一次写盘"：
   ```bash
   pnpm tools patch find '赫塔' --edits e.txt --to '<候选写法>'   # 机械生成（附带波及面）
   #  ── 逐条审 e.txt（19 条；同形词会被误伤，别照单全收）
   pnpm tools patch set --edits e.txt            # dry-run：逐条「- 现在 / + 改后」
   pnpm tools patch set --edits e.txt --write    # 一次写盘
   ```
3. `pnpm tools patch find --regex '赫塔|废柴' --kind src` 里**同一日文词根只剩一种写法**；
4. `pnpm tools patch verify` 全绿（逐字节）；
5. 玩家可见 ⇒ `release/CHANGELOG.md` 记一条。

## 非目标
* 不改 `弱者的遗迹`（`弱き者の遺跡` —— 语义已经保住，本就是它把这个问题照出来的）；
* 不改 `comment "▼G####…"` 分节标记（不是玩家可见文本）。
