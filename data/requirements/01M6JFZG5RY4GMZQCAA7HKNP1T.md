# 文档与口径漂移：M1 验收/剩余陈旧 · SCOPE 的 .lst 与 D1 冲突 · overrides addStub 名单错

- id: REQ-01M6JFZG5RY4GMZQCAA7HKNP1T
- type: req
- status: done
- parent: REQ-01M6H6FW5JGQGHACXHPWZD4Y3E
- done_reason: 文档/口径漂移已修：SCOPE 执行源=exe、菜单/模态 no-op、overrides addStub 名单、lift 的 bare 缺口改按盘上派生文件、M1 验收标注历史快照+现查、image.h 注释；平台/等待仍按用户要求只留单不改 SCOPE。
- tags: [emulator-lift, m1, docs]

## 一句话
文档 / 口径与代码漂移：M1 父单的「验收（2026-11-04）」是历史快照（ctest 10/10、A1 114/267、静态缺口 213/310），「剩余」里 S0 / S6 后四族 / S8 / A4 归一化 4 条已完成；`SCOPE.md` 的执行源与菜单 / 模态已按用户 2026-11-04 裁决修正。

## 已修（2026-11-04）
- `SCOPE.md` §4「执行源 = `.lst`」→ **拆壳件 exe**（D1；`.lst` 只作符号 / 对照 / 覆盖）——用户明确批准。
- 菜单 / 模态按 **no-op** 落地：`user32_menu.cpp` 的 `LoadMenuA` / `SetMenu` / `TrackPopupMenu` / `MessageBoxA`；
  `overrides.json` 已 `lift sync --write`。
- `overrides.md` 的 `addStub` 例外名单改正为 **10 条** = AGERC 3 + version 3 + user32_menu 4。
- `tools/lift.mjs` 的"裸缺口"不再用**期望缺口**（`gapByKey`，构造上恒 0）算，改用**盘上的派生缺口文件**：
  文件缺失 / 过期 ⇒ `bare > 0` 并报错；`report` 同一口径。
- M1 父单：验收节标注"历史快照"，新增「现查（2026-11-04 审计后）」；「剩余」按当前树重算
  （原 1/2/3/4/6 已完成；活单只剩 S6 三族与平台/win32）。
- `image.h` 的 `shim_slots` 注释改正（0x2000 = 8192 槽 / 32 KiB）。

## 判据
- `pnpm tools lift check` 绿；`pnpm tools lift report` 的 bare = 0 且现在**可**因派生文件缺失而变红。
- `requirements validate` 6/6；M1 父单正文与子节点状态一致。

## 保留
- `SCOPE.md` 的平台节 / 等待语义**未改**：那两条属于用户裁决（分别在 T6 / T1 的「待裁决」节）。
- M1 的「剩余」里 S4 逐项复核仍没做（不是文档问题，是活；未在本轮范围）。
