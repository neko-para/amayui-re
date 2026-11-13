# 🐞S3 emulator-lift · lift check 红 4 条：RaiseException / RtlUnwind 已是实现但 overrides.json 与白名单仍记成 stub

- id: REQ-01M72JFCB0W0924H3H6EQSGRK4
- type: bug
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: tools/test/lift.test.mjs#① overrides.json 逐字节等于 lift sync 的派生结果（手改一个字符就红）
- repro: tools/test/lift.test.mjs#① overrides.json 逐字节等于 lift sync 的派生结果（手改一个字符就红）
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

## 收口（2026-11-12，macOS arm64）

- `node tools/lift.mjs --sync --write`：从 C++ 注册表派生，删掉 `overrides.json` 里 `kernel32!RaiseException` / `kernel32!RtlUnwind` 两条 stub（并补上漏登记的 `agerc!_SetNameLenMax@20`）。
- `tools/lift.mjs` 的 `FAMILY_STUB_APPROVED` 删掉同两条（它们已是实现，不再是"允许继续是 stub"）。
- 读数：`node tools/lift.mjs --check` ⇒ `lift check: 绿（0 个问题）`（修前 4 个）；`node --test tools/test/lift.test.mjs` ⇒ 4/4 pass。
- 复跑命令：`pnpm tools lift check`（要 `corpus/` 的拆壳件，不进 pure 档）。
