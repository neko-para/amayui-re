# LCMapStringW 的 cchSrc=-1 只映射 1 个字符（静默截断）

- id: REQ-01M6K69K28AT5VNKZAJXT6PZJ4
- type: bug
- status: open
- parent: REQ-01M6H6FW5JGQGHACXHPWZD4Y3E
- repro: apps/emulator-lift/src/shims/kernel32_bootstrap.cpp#shimLCMapStringW
- severity: S2
- tags: [emulator-lift, m1, s6]

## 一句话
`LCMapStringW` 的 `cchSrc = -1`（NUL 结尾，CRT 最常见形态）只映射 **1 个字符**并返回 1，注释自己也写着"n<0 的路径未实现"；其余映射标志也未校验。

## 现状（2026-11-04 审计）
`apps/emulator-lift/src/shims/kernel32_bootstrap.cpp` 的 `shimLCMapStringW`：
```cpp
const std::int64_t count = n < 0 ? 1 : n;  // 只支持显式长度的路径（-1 记日志）
if (n < 0) shimLog("kernel32", "LCMapStringW", "只支持显式长度（n>0）；n<0 的路径未实现");
```
- 后果：`LCMapStringW(LOCALE_USER_DEFAULT, LCMAP_LOWERCASE, src, -1, dst, cap)` 返回 1，只转换了首字符；调用方按返回值判断"整串已转换" ⇒ **静默截断**。
- `cchDest == 0` 的查询路径用同一个 `count`，也会返回 1 而不是"整串所需字符数（含结尾 0）"。
- 只实现 `LCMAP_LOWERCASE` / `LCMAP_UPPERCASE`；`LCMAP_SORTKEY` / `LCMAP_HIRAGANA` / `LCMAP_KATAKANA` 等未处理，但没有明确失败。

## 判据（落地时写成 CTest）
- `cchSrc = -1`：`"AbC"` + `LCMAP_LOWERCASE` ⇒ 返回 4（含结尾 0），dst = `"abc\0"`；
- `cchDest = 0`：返回同一数值；缓冲不足 ⇒ `0 + 122`；
- 显式 `cchSrc = 3` 的路径保持现状（不回归）；
- 不支持的 `dwMapFlags` ⇒ `0 + ERROR_INVALID_FLAGS(1008)`（⛔ 不许静默成功）；
- 加一条变异（把 `count` 改回 `1` ⇒ 新用例红）。

## 边界
- CP932 假名的假名 / 片假名映射（LCMAP_HIRAGANA/KATAKANA）不在本单；若 M2 需要，另开并绑字符表。
