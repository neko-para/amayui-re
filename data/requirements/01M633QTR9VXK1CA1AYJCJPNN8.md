# 排版记录标志位 2（另一支字号）：置位者与 0x82 选支判据未核

- id: REQ-01M633QTR9VXK1CA1AYJCJPNN8
- type: spike
- status: open
- parent: REQ-01M5SZ3M7GCR0M23RSYBD7GEA5
- tags: [spec, text, emulator-cxx]

## 问题

`0x82` 的记录标志位 2 决定用哪一支字号；本规则正文说「由**排版层追加**时恒置起」。
本轮把**读者**和**写者**分开核，结论一半已定、一半仍未定。

## 已定（二进制可复核）

- **读者存在**：`0x4666B6` 是 `test byte ptr [eax+28h], 4`（`eax` = 记录基址），紧接着
  `0x4666EB jz` 跳过选支；置起时 `0x4666ED`–`0x466714` 会按行距模式格选另一支字体并 `SelectObject`
  （`[ebx+448]` / `[ebx+355C8]`）。⇒ `0x82` **确实**按位 2 选字号支，正文的「消费侧」成立。
- **排版层路径不置位 2**：`sub_45F090` 对第 3 实参只做 `or [ebp+arg_4], 1`（位 0，`0x45F0E5`）；
  该实参在 `sub_46BE30` 的两次 `sub_4691A0` 调用里都是文本门 `Engine+0x5EC7C`（正常 0、抑制 0x80000000）
  ⇒ 正常正文记录 flags = 0，行结束再 `|8`，没有 `|4`。

## 仍未定

- **位 2 的写者**：全 `.lst` 没有 `mov/or/and [...+28h], 4` 形态的写点。可能是**整字赋值**时带 4
  （另一个追加路径 / 文本框状态透传），也可能是 `0x7d` 一族；需要枚举 72 字节记录的**全部写者**
  （`sub_45E7E0` 的调用点）再逐一看 flags 实参。
- 若找不到写者 ⇒ 该位在本作不可达（与等距模式同形）⇒ 把 `CORE-OPS-TEXT-SPAN` 的「排版层恒置起」
  改成「消费侧存在、本作没有置位者」，并同步 `60-ops-forward`。

## 下一步

1. `pnpm tools disasm-at search --match "call    sub_45E7E0"` 枚举记录写者；对每个写者看 flags 实参。
2. 找到 `|4`（或整字 4 / 含 4 的状态值）⇒ 记 EA，补正文/绑定。
3. 找不到 ⇒ 按「不可达」裁决，改正文并留 `next` 重开条件（真机 / 采样看到 B 支字号）。

## 关联

- `REQ-01M630YH3RVEJJEGYKZJW2VQE7`（记录生产链，已收口）
- `RES-OPS-TEXT-SPAN-RECORD-FIELDS` / `RES-OPS-TEXT-SPAN-FLAG-1`
