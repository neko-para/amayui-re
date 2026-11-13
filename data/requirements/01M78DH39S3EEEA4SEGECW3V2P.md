# 🐞S3 Win32 合同缺条：`HostTextMetrics` 无 charset 字段（tmCharSet 恒 0）+ L1-GDI-01 缺该条 + 缺 WideCharToMultiByte 条款

- id: REQ-01M78DH39S3EEEA4SEGECW3V2P
- type: bug
- status: dropped
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- repro: apps/emulator-lift/src/shims/gdi32_font.cpp#shimGetTextMetrics
- severity: S3
- dropped_reason: 本单是「读档标题乱码」调查的副产物；乱码已查明是老汉化 936/GBK 存档的预期结果（见 OBS-20261113-01）。三条合同缺条（tmCharSet / L1-GDI-01 / WideCharToMultiByte）本身仍成立，但按用户 2026-11-13「只处理台账修正、关闭相关单」的决定本轮不修；要修时按正文重开。
- tags: [emulator-lift win32 contract gdi codepage]

## 现象（取证副产物，2026-11-13）

查「读档标题乱码」时发现三处**合同/字段缺条**。它们**不解释**那个缺陷（已证引擎从不读 `tmCharSet`），
但会让"实现与期望无从对齐"，属于独立的一类失效：**合同缺一条 ⇒ 实现没有可对齐的目标，错也静默**。

1. `src/runtime/font.h:92-105` 的 `HostTextMetrics` **没有 `charset` 字段**
   ⇒ `src/shims/gdi32_font.cpp:80` 只能把 `tmCharSet` 恒写 `0`（ANSI_CHARSET）。
   任何将来读 `tmCharSet` 的代码都会**静默拿到 0**，而真机上它应等于 `LOGFONTA.lfCharSet`。
2. `docs/03-emulator/contracts-win32.md:53` 的 `L1-GDI-01` **缺**「`GetTextMetricsA` 必须返回真实 `tmCharSet`」这一条。
3. `docs/03-emulator/contracts-win32.md` 只有 `L1-CP-01/02` 的 **MB→WC** 单向条款，
   **缺 `WideCharToMultiByte`**（CP932 方向）的条款 —— 而它是"宽串写回窄串"的唯一通道。

## 要做

1. `HostTextMetrics` 加 `std::uint8_t charset`（`font.h`）；`font.cpp` 的装配处填 `out->charset = desc.charset;`；
   `gdi32_font.cpp:80` 写 `tm.charset`（⛔ 不再恒 0）。
2. `L1-GDI-01` 补该条，可核验判据：设一个 CJK 字体 ⇒ `tmCharSet` 必须非 0 且等于 `lfCharSet`。
3. 补 `WideCharToMultiByte` 的合同条款（与 `L1-CP-02` 对称）：`cbMultiByte == 0` 探长、
   `cchWideChar == -1` 含 NUL、缓冲不足返回 0 **且不写**、`lpUsedDefaultChar` 语义、非法码点的替换行为。

## 判据

- `tmCharSet` 不再是常数 0：pure CTest（设 CJK 字体 ⇒ `tmCharSet == lfCharSet`）**修前红**；
- `contracts-win32.md` 两条缺条补上，`L1-` 编号不与既有冲突（`pnpm test` 的合同守卫要绿）；
- ⛔ 不许只改注释/只改字段而不断言。

## 不在本单

- ⛔ 不解释「读档标题乱码」：那条线的结论是**未取证**（标题在压缩+加密的体里，是 8-bit MSVC `std::string`；
  292 B 头无标题字段）。下一步是插桩看标题到字形层是 3 个还是 6 个码位。
