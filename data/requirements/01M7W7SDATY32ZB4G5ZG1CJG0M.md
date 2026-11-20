# P5 · 注册/死件清理（注释 / 样板 / cmake 诊断）

- id: REQ-01M7W7SDATY32ZB4G5ZG1CJG0M
- type: req
- status: open
- parent: REQ-01M7W7RSC34YJTNMK3ESYVXZY2
- tags: [emulator-lift, tests, cmake]

## 做什么

- 删/改 `subprocess_skip.h`、`stress_skip_report.h:8-11` 的过期注释；`stress/skip-report` 的合成文本负控
  **留在 `LIFT_STRESS` 档**（D5，只改注释）；
- `tests/cmake/lift_aot_split.cmake` 只留手工诊断说明；
- 消除 `target_link_libraries` 样板（⛔ 不动 `add_test` 的注册位置 / 名称）。

## 判绿

- `cmake-test-entry` 90+ 条全绿；`ledger.test.mjs` 的 pure/assets 档不变。
- `ctest -N` 名称集合不变（除 P1 的 `support-child-proc` 与 P4 的 `support-bmp`）。
- `pnpm verify` 绿。

## 边界

- ⛔ 不把 `add_test` 收进函数/宏（分档判据会错、`cmake-test-entry` 解析不到）。
