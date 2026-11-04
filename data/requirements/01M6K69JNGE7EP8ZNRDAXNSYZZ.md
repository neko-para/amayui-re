# GetStringTypeW 的 CT_CTYPE1 位抄错且忽略 dwInfoType（CRT 宽字符分类静默失真）

- id: REQ-01M6K69JNGE7EP8ZNRDAXNSYZZ
- type: bug
- status: done
- parent: REQ-01M6H6FW5JGQGHACXHPWZD4Y3E
- verify: apps/emulator-lift/tests/locale_ext.cpp#get-string-type-w-ctype1
- repro: apps/emulator-lift/src/shims/kernel32_bootstrap.cpp#shimGetStringTypeW
- severity: S2
- tags: [emulator-lift, m1, s6]

## 一句话
`GetStringTypeW(CT_CTYPE1)` 的位值抄错、`dwInfoType` 被 `(void)type` 丢弃 ⇒ CRT 的宽字符分类
（`iswalpha` / `iswspace` / `iswdigit`）**静默**得到错结果。**已修 ASCII 部分**。

## 改了什么
`src/shims/kernel32_bootstrap.cpp#shimGetStringTypeW`：
- CT_CTYPE1 位按 MSDN 表：UPPER 0x1 / LOWER 0x2 / DIGIT 0x4 / SPACE 0x8 / PUNCT 0x10 / CNTRL 0x20 /
  BLANK 0x40 / XDIGIT 0x80 / ALPHA 0x100。定点：`'A'=0x0181`、`'a'=0x0182`、`'G'=0x0101`、
  `'0'=0x0084`、`' '=0x0048`、`'!'=0x0010`；`U+0000` / `U+000D` = CNTRL(0x20)
  （旧实现给 NUL `0x0001` = C1_UPPER）。
- `dwInfoType != CT_CTYPE1` ⇒ `0` + `ERROR_INVALID_PARAMETER(87)` + 一行 `shimLog`。
- `cchSrc == -1`（NUL 结尾）现在真的按 NUL 扫（旧实现直接失败）；`cchSrc == 0` / `< -1` ⇒ 0 + 87。
- **非 ASCII**：位给 0 + 一次日志（⛔ 不拿 `w < 0x80` 猜）。实测启动路径就会碰到 84 个非 ASCII 字符，
  所以这条缺口是**真缺口**，已另开 `REQ-01M6KH7EPPNWJDHPDQ6REV2RRM`（同级单）做字符类表。

## 与单里写法的两处更正
- `CT_CTYPE2 / CT_CTYPE3` 的值是 **2 / 4**（单里写"5/6? 具体值按 Win32"，`<待核>`）。
- 这两个值在**真机上合法**（会成功）。本单按判据要求返回 87 = "本模拟器没实现"的**明确失败**，
  ⛔ 不是文档行为 —— 将来实现它们时这条要改。

## 判据（已落成 CTest）
- `apps/emulator-lift/tests/locale_ext.cpp#get-string-type-w-ctype1`
  A/a/G/0/9/!/空格 逐位相等 + NUL/CR = CNTRL + `cchSrc=-1` + 非 ASCII = 0（缺口钉住，实现表后要改）。
- `apps/emulator-lift/tests/locale_ext.cpp#get-string-type-w-info-type`
  `dwInfoType ∈ {0,2,4,3}` ⇒ 0 + 87；空指针 / `cchSrc=0` / `cchSrc=-2` ⇒ 0 + 87。

## 证据
- 变异实测：`kC1Alpha` 改成 `0x0800` ⇒ `get-string-type-w-ctype1` 红（`0x0181` 变成 `0x0881`）。
