# 跨规则命名统一：Scene+0xB5B0 在三条规则里叫三个名字

- id: REQ-01M6DAN9C279GEK1E2NC1D5CTN
- type: req
- status: done
- parent: REQ-01M6DAMF1MN58WVR07ZYMQ8FCP
- done_reason: 统一名已定为「强制完成」位（独立的一格、旧称中止位只作别名）；三条规则 + wait/list/texture/render + bindings 已对齐；RES-ENGINE-CELL-238-READER 收窄为写者相位；独立复审无硬矛盾。
- tags: [spec, naming]

## 裁决（2026-11-02）

- 统一名 = **「强制完成」位**；读侧的效果是**中止当前等待**，旧称「中止位」只作**别名**（`70-vm.md` 的规范句里保留一句别名说明，⛔ 不许实现成两格）。
- 三条规则的说法已统一：`CORE-OPS-ENGINE-CELL-238`（A 格 → **强制完成位**）、`CORE-TIMING-WINDOW`（**「强制完成」位**）、`CORE-OPS-SCENE-CLEAR`（四个标记之一 → **强制完成位**）。
- 同批把 `60-ops-list` / `60-ops-wait` / `60-ops-texture` / `60-ops-render` / `bindings.json` 的「档 / 中止位」统一成「位 / 强制完成位」。
- `RES-ENGINE-CELL-238-READER` 收窄为「A 格的**写者相位**未枚举；读者已定（与 1 比较，等则清 B / C）」。

## 判据

- 除 `70-vm.md` 的别名说明外，`data/spec` 全树没有「中止位」作为使用名；
- 四个场景级标记 = 脏 + 强制完成位 + 另外两个（「有动画」不在四个里）；
- `pnpm tools spec validate` + 独立复审（只读 `data/spec/`）绿。
