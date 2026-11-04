# LCMapStringW 的非 ASCII 映射（假名 / 全角 / SORTKEY / BYTEREV）

- id: REQ-01M6KH7HC3ZP3M5RS8EJ3813WC
- type: req
- status: open
- parent: REQ-01M6H6FW5JGQGHACXHPWZD4Y3E
- tags: [emulator-lift, m2, locale]

## 一句话
`LCMapStringW` 现在只做 **ASCII** 的 `LCMAP_LOWERCASE` / `LCMAP_UPPERCASE`（`REQ-01M6K69K28AT5VNKZAJXT6PZJ4`
已收口：`cchSrc=-1` 映射整串、`cchDest=0` 问大小、未实现的标志明确失败）；假名 / 全角 / 排序键
那几档一律 `ERROR_INVALID_FLAGS(1008)` —— **明确失败**，不是静默成功。

## 剩余
1. CP932 的假名 / 全角映射：`LCMAP_HIRAGANA(0x00100000)` / `LCMAP_KATAKANA(0x00200000)` /
   `LCMAP_HALFWIDTH(0x00400000)` / `LCMAP_FULLWIDTH(0x00800000)`。
   需要与 `GetStringTypeW` 那条同源的字符表（⛔ 不许逐字符硬编码）。
2. `LCMAP_SORTKEY(0x00000400)`：返回的是**排序键字节串**（不是宽字符），语义与其余标志不同
   （返回值是字节数、`cchDest` 是字节容量）；要么实现，要么维持明确失败。
3. `LCMAP_BYTEREV(0x00000800)`：字节序反转，语义独立。
4. 非 ASCII 的大小写（拉丁扩展 / 希腊 / 西里尔）：现在只映射 ASCII `A-Z` / `a-z`。

## 判据（落地时写成 CTest）
- HIRAGANA / KATAKANA 各一条往返（あ⇄ア）；
- FULLWIDTH / HALFWIDTH 各一条往返（A⇄Ａ）；
- 失败路径保持：不认识的位 ⇒ 0 + `ERROR_INVALID_FLAGS(1008)`（现在的
  `locale-ext#lcmap-string-invalid-flags` 断言的就是这一条，实现后要跟着收窄）。
