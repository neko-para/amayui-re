# 守卫：spec 正文与 audit 残差文本里的 id 引用必须存在（悬空引用）

- id: REQ-01M69ARWA623EAAVNCTPPXMM46
- type: req
- status: dropped
- parent: REQ-01M674XHSX2HSFWSKDWZV2C63D
- dropped_reason: 内容（悬空引用守卫 + 4 个历史悬空 id）已并入交接单 REQ-01M5BK8QMQYK7XQG707YVMQ202 的「待基建」；按用户口径不单独留单。
- tags: [spec residual tooling]

# 守卫：spec 正文与 audit 残差文本里的 id 引用必须存在

## 为什么会有这张单

2026-10-31 的 TITLE 足迹残差收口把 46 条残差改写成合同并从 `audit.json` 删除。收口后两路独立复审各自撞到同一类问题：
**删了残差，但别处还在引用它** ——

- `06-window.md` 三处引用已删除的 `RES-FRAME-EXIT-BOOT`（两处正文 + 一处残差清单）；
- `40-pools.md` 两处「字符串赋值路径…见残差」实际指向已收口的 `RES-STRING-SET-LOCAL`；
- `FMT-AGF-HEADER / RES-AGF-BMP` 的 `what` 仍写「同 `RES-TEX-LOAD-BMP`」（该条已收口）；
- `62-adv-text.md` 正文出现已删除的 `RES-ADV-TEXT-LAYER-TIMING` 并标注「已收口」。

`node tools/spec.mjs --validate` **看不见**这类问题：它的自包含不变量只查"正文里有没有机器坐标 / 台账 id / 守卫路径"，
不查"正文引用的 `RES-…` / `GAP-…` 是否还存在"。⇒ 结果是悬空引用只能靠人读到。

## 目标（可判定）

给 `spec validate` 增加一条不变量（或一个独立守卫 `tools/test/spec-refs.test.mjs`）：

1. **章节正文**里出现的每个 `RES-…` / `GAP-…` id 都必须能在 `audit.json`（规则的 `residual[]` 或 `gaps[].residual[]` / `chapters[].residual[]`）里解析到；
2. **audit 的残差文本**（`what` / `next` / `reason` / `impact`）里提到的 id 同样必须存在（允许白名单式豁免：文本明确写"已并入本条并删除"的历史说明）；
3. 报错要给出 `文件:行` + 悬空 id + "它现在在哪条规则下"（若已并入别的条，提示改成指向该规则 id）。

## 判据

- 守卫**能红**：把某条已存在的残差 id 从 `audit` 删掉但保留正文引用 ⇒ 红；把引用改回存在 id ⇒ 绿。
- 一条 `mutate-check` 变异（`tools/mutate-check.mjs` 清单加一条）。
- 不与"正文不许写内部材料"冲突：`RES-…` 这类 id 本来就允许出现在正文（它是章节间的指针），只是必须指得到。
- `pnpm tools spec validate` 与 `pnpm test` 全绿。

## 非目标

- 不检查 id 的**语义**是否仍然成立（那要靠 `next` 与复审）；
- 不要求把历史记录（`docs/02-engine/spec-audit-*.md`）也纳入。

## 怎么查 / 怎么改

- 现成的机械检查脚本（一次性、已删）：按 `data/spec/spec.json` 的章节顺序读 `chapters/*.md`，正则 `\b(RES|GAP)-[A-Za-z0-9-]+\b`，
  与 `audit.json` 里的 id 全集比对；audit 侧同理扫 `what` / `next` / `reason` / `impact`。
- 不变量落在 `tools/lib/spec.mjs` 的 validate 流程（与 #8 自包含同一处），守卫用例落 `tools/test/spec-refs.test.mjs`（`@env pure @kind gate`）。
