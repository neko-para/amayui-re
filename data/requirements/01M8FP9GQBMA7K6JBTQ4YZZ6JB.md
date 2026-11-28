# 插件 v2 收尾：scenario/lib 迁移 · C5 dispatch · perf A/B · FIELD 体验 · MSVC

- id: REQ-01M8FP9GQBMA7K6JBTQ4YZZ6JB
- type: req
- status: doing
- parent: REQ-01M7JHSFB2SSBBNSJ82YDED8M1
- tags: [plugin, emulator-lift]

## 为什么

插件 v2 的**最终形态**已落地（2026-11-28 用户裁决）：scenario/lib/真实入口全部迁 v2 并改为 `.ts`，
v1 运行时/兼容层已删除，`plugin.d.ts` 只剩 v2。本单只剩两项"需要独占机器/人工"的收尾。

## 收什么

1. **perf A/B**：1 条永不命中 hook ≤1.5%（20 轮 CI 上界）、cheap key 档 ≤ 今天读点 poll、JS 回调每帧上限。
   判据 = rel / 同机 / 交错 ≥3 对 / 中位（AGENTS §12.6）；读数进 `.tmp/` 文档，⛔ 不进 test。
2. **FIELD 人工体验**：`--user-run` + `out-user/`（用户侧），agent 不把它当判据。

## 判据

- perf：命令 + 读数文件（rel、win32、交错 ≥3 对、中位）；⛔ 不写"全绿"。
- FIELD：用户本人跑完给结论；agent 只提供命令与复现素材。

## 怎么查

- 最终 API 合同：`docs/03-emulator/plugin-runtime.md`（v2 面）
- 迁移证据：`.tmp/plugin-v2/final/{plan,p1-report,p1-review,p2a-report,t1-report}.md`
