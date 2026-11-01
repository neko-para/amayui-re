# 十六进制立即数只认前导 0：12Ch / 100h 一律判成未知（语料 7706 行）⇒ 块搬移宽度与循环上界静默少解

- id: REQ-01M6BSEPMV2KTMPFQXTAD4JF1D
- type: bug
- status: done
- parent: REQ-01M6BPB07YQ73NYVMYNFPQ1X26
- repro: tools/test/shape-extents.assets.test.mjs#★ T3 块搬移的宽度
- severity: S2
- tags: [shape tooling]

## 一句话

`evalOperand` 只认 `^0(… )h$` 形式的十六进制立即数，而 IDA 只在**以字母开头**时补前导 0
（`0B9h`），以数字开头时**不补**（`12Ch` / `100h` / `1Fh`）⇒ 这类立即数一律被判成**未知**。
症状不是报错，而是**静默少解**：实测语料 7706 行这种形态。

## 影响（可观察）

- 块搬移的计数解不出：`sub_41ED80` 的 `mov ecx,12Ch` → `rep movsd` 的宽度从 1200 B 变成"判不了"；
- 循环上界 / `mov ebx,100h` 这类常量同理（索引 / 步长链少一截）；
- 报告里只表现为"判不了"或残差变多 —— 不会红。

## 复现（修前）

`node tools/shape.mjs --extents sub_41ED80` ⇒ `0x41edfc rep movsd` 的 `bytes=null`；
`node tools/shape.mjs --access <fn>` 里 `mov ecx, 12Ch` 之后的 `ecx` 是 `unk`。

## 修法

立即数正则改成 `^([0-9][0-9A-Fa-f]*)h$`（两种写法都认）。⛔ 只改解析，不改任何判据。

## 判据 / 守卫

`tools/test/shape-extents.assets.test.mjs` 的 1200 B（`0x12C × 4`）；变异"退回只认前导 0" ⇒ 该守卫红。
入库模型上守恒③ 仍闭合（变化是"少解变多解"，不是桶消失）。
