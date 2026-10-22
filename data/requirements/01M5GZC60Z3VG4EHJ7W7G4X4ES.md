# 启动窗口 / 渲染请求边界成篇（06-window / 60-ops-render）

- id: REQ-01M5GZC60Z3VG4EHJ7W7G4X4ES
- type: req
- status: done
- parent: REQ-01M59B6Y8VGCZJN79XVQGHB390
- verify: tools/test/spec.test.mjs#★ 真源：data/spec/{spec,bindings,audit}.json + chapters/*.md 过全部不变量，且试点未登记 = 0

## 判据

- `data/spec/chapters/06-window.md`（5 条 `CORE-BOOT-*`）与 `60-ops-render.md`（3 条 `CORE-OPS-RENDER-*`）已进 manifest / audit.chapters / bindings / audit.rules，且三份文件按 rule id 一一对应。
- `pnpm tools spec validate` 13/13 绿；`pnpm tools spec report` 的审计缺口与 opcode 划分不因新增规则破缺（0x204 / 0x20d / 0x33f 已从 gaps 移出）。
- `tools/test/spec-window-render-sites.assets.test.mjs` 存在且能因站点 EA 偏移而红（`node tools/mutate-check.mjs --match spec-window` 已验证）。
- 独立只读 `data/spec/` 的复审已完成；S1 矛盾已修，剩余未知项在规则残差里逐条带 `next`。

## 范围 / 非目标

- 只到**引擎发出的请求 + 参数 + 时序**：窗口过程逐条消息处置、D3D 呈现参数逐位映射、像素 / 混音结果不在本单。
- `presentation` 仍是未闭环机制；本单不声称它闭环。
