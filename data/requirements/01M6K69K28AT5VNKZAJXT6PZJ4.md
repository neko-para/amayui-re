# LCMapStringW 的 cchSrc=-1 只映射 1 个字符（静默截断）

- id: REQ-01M6K69K28AT5VNKZAJXT6PZJ4
- type: bug
- status: done
- parent: REQ-01M6H6FW5JGQGHACXHPWZD4Y3E
- verify: apps/emulator-lift/tests/locale_ext.cpp#lcmap-string-null-terminated
- repro: apps/emulator-lift/src/shims/kernel32_bootstrap.cpp#shimLCMapStringW
- severity: S2
- tags: [emulator-lift, m1, s6]

## 一句话
`LCMapStringW` 的 `cchSrc = -1` 只映射 **1** 个字符并返回 1 ⇒ 调用方按返回值判断"整串已转换"，
**静默截断**。**已修**；非 ASCII 的映射另开 `REQ-01M6KH7HC3ZP3M5RS8EJ3813WC`（同级单）。

## 改了什么
`src/shims/kernel32_bootstrap.cpp#shimLCMapStringW`：
- `cchSrc == -1`：扫到 NUL 映射整串（含把结尾 0 写进 `lpDestStr`），返回值 = 字符数**含**结尾 0
  （`"AbC"` ⇒ 4、dst = `L"abc\0"`）。
- `cchDest == 0` 是问大小（返回同一数值，不碰 `lpDestStr`）；`cchDest < 0`、或 `lpDestStr == 0`
  而 `cchDest > 0` ⇒ `0` + 87；`cchDest < 需要` ⇒ `0` + `ERROR_INSUFFICIENT_BUFFER(122)`。
- 未实现的 `dwMapFlags`（SORTKEY 0x400 / BYTEREV 0x800 / HIRAGANA 0x100000 / KATAKANA /
  HALFWIDTH / FULLWIDTH …），以及 `LCMAP_LOWERCASE|LCMAP_UPPERCASE` 同时给
  ⇒ `0` + `ERROR_INVALID_FLAGS(1008)`（⛔ 旧实现静默按"无映射"成功）。
- `dwMapFlags == 0` 是合法的"不映射"（原样复制 + 同一个返回值）。
- 显式 `cchSrc = 3` 的返回值也从 1 改成 **3**（MSDN：返回映射后的字符数）。这比单里写的
  "保持现状"更贴文档；**映射的字符数没变**（3 个都映射），所以不是回归。

## 判据（已落成 CTest）
- `apps/emulator-lift/tests/locale_ext.cpp#lcmap-string-null-terminated`
  `cchSrc=-1` ⇒ 4 / `dst="abc\0"`；`cchDest=0` ⇒ 4；缓冲不足 ⇒ 0 + 122；显式 3 ⇒ 3（不回归）；
  UPPERCASE 同口径。
- `apps/emulator-lift/tests/locale_ext.cpp#lcmap-string-invalid-flags`
  0x400 / 0x800 / 0x100000 ⇒ 0 + 1008；0x100|0x200 ⇒ 0 + 1008；`dwMapFlags=0` ⇒ 原样复制、返回 4。

## 证据
- 变异实测：把 `count` 改回 1 ⇒ `lcmap-string-null-terminated` 红（4 变 1、`dst` 只剩首字符）。
- 仍缺（同级的另一单）：CP932 假名 / 全角的 HIRAGANA / KATAKANA / HALFWIDTH / FULLWIDTH、
  `LCMAP_SORTKEY`（返回字节串语义不同）、`LCMAP_BYTEREV`、非 ASCII 大小写。
