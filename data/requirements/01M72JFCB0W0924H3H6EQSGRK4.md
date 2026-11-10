# 🐞S3 emulator-lift · lift check 红 4 条：RaiseException / RtlUnwind 已是实现但 overrides.json 与白名单仍记成 stub

- id: REQ-01M72JFCB0W0924H3H6EQSGRK4
- type: bug
- status: open
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- repro: data/lift/overrides.json#kernel32!RaiseException
- severity: S3
- tags: [emulator-lift, lift-check, overrides]

## 一句话
`pnpm tools lift check` **红**（4 条），且**不是本轮改动引起的**：`kernel32!RaiseException` / `kernel32!RtlUnwind` 在 C++ 里已经是**实现**（`add`，`apps/emulator-lift/src/shims/kernel32_bootstrap.cpp` + `src/runtime/seh.cpp`），而 `data/lift/overrides.json` 与 `FAMILY_STUB_APPROVED` 白名单里仍记成 `stub`。

## 读数（2026-11-10，本机）
```
$ pnpm tools lift check
  ✗ kernel32!RaiseException：C++ 里是**实现**（add），JSON 却写成 stub —— 要么删条目，要么在 C++ 用 addStub
  ✗ kernel32!RtlUnwind：C++ 里是**实现**（add），JSON 却写成 stub —— 要么删条目，要么在 C++ 用 addStub
  ✗ FAMILY_STUB_APPROVED 里的 kernel32!RaiseException 不是（导入的）stub —— 白名单漂了（删掉它或说明）
  ✗ FAMILY_STUB_APPROVED 里的 kernel32!RtlUnwind 不是（导入的）stub —— 白名单漂了（删掉它或说明）
lift check: 4 个问题（红）
```
`git status --porcelain data/lift/` = 空（本轮没动它）；`overrides.json` 最后改动 = 提交 `2cafab2`，`seh.cpp` = 提交 `13d9ef7` ⇒ **入口实现先落地、清单没跟上**。

## 判据（修好后）
`pnpm tools lift check` 退出码 0，且 `overrides` 里 RaiseException / RtlUnwind 的 `kind` 与 C++ 的 `add` 一致（或删条目 + 白名单同步）。

## 未做
- 未改 `data/lift/overrides.json`：属另一条线（平台清单），本轮只登记不改（"不顺手改无关文件"）。
