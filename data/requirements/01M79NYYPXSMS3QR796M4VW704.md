# 🐞S3 emulator-lift · close-confirm 全量跑偶发红（43 s 等不到 TITLE.BIN marker 边沿），单跑即绿

- id: REQ-01M79NYYPXSMS3QR796M4VW704
- type: bug
- status: open
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- repro: apps/emulator-lift/tests/close_confirm.cpp#marker edge: cur=1 id=0x00005264 name=TITLE.BIN
- severity: S3
- tags: [emulator-lift, test-flake, interaction]

## 现象

2026-11-13 一次全量 `ctest`（macOS arm64 / RelWithDebInfo / HEAD 工作树）里 `close-confirm` **红一次**：
等了 43 s 没等到 TITLE.BIN 的 marker 边沿，单跑即绿；第二次全量 93/93 全绿。

## 证据

- 失败签名与 `tests/close_confirm.cpp` 的判据 ② 一致：`marker edge: cur=1 id=0x00005264 name=TITLE.BIN ⇒ close_window`
  没出现（`contains(...)` 红），也就是**边沿采样窗口**没盖住那一次 TITLE 重载帧。
- 同族先例：`REQ-01M78A0V0X1X1PCK7MP71Q1J81`（quit-graph step3 首次点击偶发丢）与
  `REQ-01M786CQP4JT8D7CSJ64NG6KYC`（慢机固定超时）——**根因类别相同**：判据要的是"边沿"，而窗口是固定值。

## 判据

- 反复跑（≥10 次）不再红，且"等不到边沿"时**报出**当前身份 / 已等待帧数 / 最后一次采样，而不是只报 43 s；
- ⛔ 不许靠调大超时或重试一次过；⛔ 不许把断言放宽成"看到 TITLE 就行"。

## 非目标

- 不改 close_window 的注入语义；不动 `_ShowDialog@12` 的返回值。
