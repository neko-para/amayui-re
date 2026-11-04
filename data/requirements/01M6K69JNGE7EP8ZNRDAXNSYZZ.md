# GetStringTypeW 的 CT_CTYPE1 位抄错且忽略 dwInfoType（CRT 宽字符分类静默失真）

- id: REQ-01M6K69JNGE7EP8ZNRDAXNSYZZ
- type: bug
- status: open
- parent: REQ-01M6H6FW5JGQGHACXHPWZD4Y3E
- repro: apps/emulator-lift/src/shims/kernel32_bootstrap.cpp#shimGetStringTypeW
- severity: S2
- tags: [emulator-lift, m1, s6]

## 一句话
`GetStringTypeW(CT_CTYPE1)` 的位值抄错，且 `dwInfoType` 被 `(void)type` 丢弃：CRT 的宽字符分类（`iswalpha` / `iswspace` / `iswdigit` …）会**静默**得到错结果。

## 现状（2026-11-04 审计）
`apps/emulator-lift/src/shims/kernel32_bootstrap.cpp` 的 `shimGetStringTypeW`：
```cpp
if (w == 0) flags |= 0x0001;                       // 0x0001 = C1_UPPER（不是"NULL"）
if (w < 0x20) flags |= 0x0020;                     // C1_CNTRL（对）
else if (w == 0x20) flags |= 0x0020 | 0x0008;      // 空格被标成 CNTRL|SPACE
else if (w >= 0x30 && w <= 0x39) flags |= 0x0004 | 0x0080;  // DIGIT|XDIGIT（对）
else if (A-Z / a-z) flags |= 0x0004 | 0x0100;      // 字母被标成 DIGIT|ALPHA
```
- CT_CTYPE1 的正确位：`C1_UPPER 0x0001` · `C1_LOWER 0x0002` · `C1_DIGIT 0x0004` · `C1_SPACE 0x0008` · `C1_PUNCT 0x0010` · `C1_CNTRL 0x0020` · `C1_BLANK 0x0040` · `C1_XDIGIT 0x0080` · `C1_ALPHA 0x0100`。
- 按 MSDN：`'A'` = `UPPER|ALPHA|XDIGIT` = `0x0181`；`'a'` = `LOWER|ALPHA|XDIGIT` = `0x0182`；`'G'` = `0x0101`；`'0'` = `0x0084`；`' '` = `SPACE|BLANK` = `0x0048`；`'!'` = `0x0010`。
- `dwInfoType` 只接受 CT_CTYPE1（1）；CT_CTYPE2/3（5/6? 具体值按 Win32）目前会**假装成功**并按 CT_CTYPE1 写错位。

## 影响（为什么不是"已知边界"）
- 它属于 D6 的"码页与字符串"族，且**登记为 implemented**；审计把它列为 20 条 thin-suspect 之一。
- 启动到 L2 不依赖（A4 仍绿），但 M2 的文本 / 字符串比较会直接踩；错误是静默的（返回 1）。

## 判据（落地时写成 CTest）
- ASCII 定点：`A/a/G/0/9/!/空格` 逐位相等（含 XDIGIT 的边界）；
- `dwInfoType` 不支持的类别 ⇒ `0 + ERROR_INVALID_PARAMETER(87)`，⛔ 不许静默按 CT_CTYPE1 处理；
- 非 ASCII（CP932 假名 / 全角）单列 `<待核>`：需要一张字符类表，不能拿 `w < 0x80` 猜；
- 加一条变异（把 `C1_ALPHA` 位写错 ⇒ 新用例红）。

## 边界
- 本单只保证 **ASCII 的 CT_CTYPE1 位正确 + 不支持的 info type 明确失败**；CP932 的字符分类另开（若 M2 需要）。
