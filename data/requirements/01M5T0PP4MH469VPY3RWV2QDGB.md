# spec 修正：执行区间的两个时间戳格 = 场景时钟那一对（原文说『本帧记录里的两格、与场景不是同一对』）

- id: REQ-01M5T0PP4MH469VPY3RWV2QDGB
- type: req
- status: done
- parent: REQ-01M5SZ3M7GCR0M23RSYBD7GEA5
- done_reason: spec 正文 + 残差 + falsifier 已按语料对齐（两轮取证、四轮独立复审，均只读 data/spec）；门禁 spec validate 13/13、report 未登记 0、spec.test.mjs 全绿。审计记录见 docs/02-engine/spec-audit-2026-10.md 六十四节；未决项已全部登记为具名残差（各有 next）。

## 判据

- `pnpm tools spec validate` 13/13、`pnpm tools spec report` 0 未登记、`node --test tools/test/spec.test.mjs` 全绿。
- 正文改动行不含 EA / 偏移 / `sub_XXXXXX` / 台账 id（自包含不变量）。
- 每条"本版不规定 / 未取证"都指向**具名残差**，且该残差有具体 `next`；不新增"继续调查"式空话。

## 为什么要改（判据）

`0x1f4` / `0x20c` / 场景推进写的是**同一对固定偏移**：
`0x41A0C9` / `0x41A0CF` / `0x41A0D5`（`0x1f4`）、`0x41A1CA` / `0x41A1D0` / `0x41A1D6`（`0x20c`）、
`0x41278D` / `0x41279F`（场景，`T_Scene@0x4ED10 + 0xB5A4 = 0x5A2B4`）。
而原文写「本帧记录里的两格、与场景不是同一对」⇒ 与语料冲突，也与 `60-ops-render.md` 的「这两格是引擎级的，不是帧记录里的那一对」自相矛盾。

## 处置

- `70-vm.md`：`:19` / `:27` / `:123` / `:125` / `:132` / `:221` 按「同一对场景时钟；本版登记的写者都先搬旧再写新」对齐；`:213` 的状态对应表行删除（`0x1f5` / 泵不读这对格）。
- `60-ops-render.md`：`:180` / `:181` / `:219` 同口径；新增「本帧场景时刻取覆盖前还是覆盖后未取证」这一句。
- `audit.json`：三条残差改写 + 新增 `RES-RENDER-STEP-SCENE-NOW`；`CORE-OPS-RUN-BRACKET` 的 falsifier 换成两条可观察量。
- `bindings.json`：`0x41a1ca` / `0x41a1d6` 两条 site 说明从"帧…时刻"改成"引擎…时刻"（EA 未动）。

## 复审（四轮，全部只读 `data/spec/` 的独立 subagent）

1. 第一轮：6 条自相矛盾（其中 213 行、19 行由本轮改动引入/暴露）；
2. 第二轮：6 条未收口 + 5 条新矛盾（区间忙 vs 主循环「暂停」、覆盖后「本帧场景时刻」无定义、同一格三套名字）；
3. 第三轮（有界判据）：剩 3 处一行级；
4. 第四轮：复核那 3 处（结论见 `docs/02-engine/spec-audit-2026-10.md` 六十四节）。

## 未决（已登记为残差，各带具体 next）

- `0x23c` 的刷新点无合同（只出现在 `GAP-OPS-0200-02FF`）；
- 区间忙与主循环「暂停」是否同一状态；
- `0x20c` 覆盖后「本帧场景时刻」取哪次采样；
- 「上一步」格有没有读者。
