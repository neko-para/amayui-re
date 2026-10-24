# 残差第十四批：存档写侧块布局落地

- id: REQ-01M5N9CP5MYCXPF7VSN58FNQMQ
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: tools/test/emulator-new-save-codec.test.mjs#★ 块 1 = 浮点 raw（与输入逐位一致）；块 3 长度为 0（尾段紧接字符串段）
- tags: [spec residual]

# 残差第十四批：存档写侧块布局落地

## 一句话

把六块合同落到 `buildSaveBody` 写侧：浮点写块 1（不再写块 3）、引用三池计数与块长为 0、`tailBase` 预留洞、v1/v2 门控；并修正"块 0 是明文"的正文自相矛盾。

## 代码

- `c0=ints`、`c1=floats`、`c2=strings`、`c3=c4=c5=0`；块 0 整型（`encodeInt`）、块 1 浮点 raw、块 2 字符串；块 3/4/5 零长。
- `tailBase = blocksBase + 4*(c0+c1+capWords+1)`（引用池启用时再加 `4*(c3+c4+c5)`）。
- 浮点内容门控 `v1 !== 2 && v2 >= 20`；不写时留全 0 洞。
- 正文：块 0 文件内容是编码后的值（内存池明文），写侧编码、读侧解码。

## 守卫 / 变异

`emulator-new-save-codec` 8 条（计数槽 / 块 1 逐位 / 块 3 零长 / v2<20 / v1=2 / CRC / 变换 / 信封）；变异 +3（331 条）。

## 仍开着

读侧还原 / 整池重编码 · `rec[259]` 表下标 · 引用三池内容 · 尾段场景表 · `createDevice` backBuffer · 真实绘制 S2+。

## 判据

`pnpm test` 998/0 · `pnpm test:assets` 201/0（1 skip）· `pnpm test:mutation` 331/331 · `pnpm tools spec validate` 13/13 · `pnpm tools requirements validate` 6/6 · `pnpm typecheck` 六工程绿。

## 怎么查

`pnpm tools spec show FMT-SAVE-CODEC` · `docs/02-engine/spec-audit-2026-10.md` §31 · `tools/test/emulator-new-save-codec.test.mjs`。
