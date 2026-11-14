# 🐞S2 emulator-lift · 两条 assets e2e 自愿传 --present-driver software ⇒ 0.4 fps 慢机假红（REQ-01M786CQ 根因过时）

- id: REQ-01M7AQRVP2MNZ4PSZCS8DMEHEZ
- type: bug
- status: dropped
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- repro: docs/03-emulator/observations.md#OBS-20261114-01
- severity: S2
- dropped_reason: 同上（测试模型改造 REQ-01M7ATAB…）：两条自愿传 --present-driver software 的 assets e2e 已删除 —— rg 'present-driver' 全仓零命中（2026-11-14 核）；那个驱动的 0.4 fps 前提也随 CPU 光栅退休消失（--raster 已删）⇒ 载体与根因都不存在。
- tags: [emulator-lift, ctest, e2e]

## 现象

两条 assets e2e 用例**自愿**传 `--present-driver software`：

- `apps/emulator-lift/src/main.cpp:644-648` 对这个选择会打警告"这是**软件渲染**，不是 GPU 路径"；
- 测试侧：`apps/emulator-lift/tests/close_confirm.cpp:180`（★ 原同案的 `agerc_ui_state.cpp` 已于 2026-11-13 整条删除 —— 它断言的正是我们自己 stub 的返回值）
  （`tests/save_graph.cpp:184-185` 已经不传 —— 可作对照）。

后果：软件渲染下 ~0.4 fps，用例在慢机上**固定超时红**。

## 为什么现在开单

`REQ-01M786CQP4JT8D7CSJ64NG6KYC`（"Windows 慢机：assets 档 e2e 固定超时太短"）的**根因写得过时**：
它说这几条用 `--raster cpu`，而 `--raster` 已被删除（`lift/raster-flag-gone` 这条用例本身就在守这件事）。
真正的原因是测试**自己选了软件渲染器**，不是机器没有 GPU、也不是超时太短。

## 判据

- 两条用例在 GPU 可用时走 GPU（不传 `--present-driver software`），并删掉 `main.cpp` 里那条
  "你选了软件渲染"的警告在测试路径上的依赖（若它只服务测试）；
- `REQ-01M786CQ…` 的正文按新根因改写（它的"调大超时"口径应作废）；
- 证伪：若 `--present-driver software` 是**故意**用来覆盖软件路径的（给出出处/用例注释），本条不成立 —
  那时应改为"软件路径单独一条 opt-in 用例，不混在 e2e 里"。

## 非目标

- ⛔ 不改超时值来换绿。

## 实测补充（2026-11-13，Windows x64）

默认档 `ctest`（排除 `overlay-window-smoke`）读数：95 条 / 470.78 s，
其中 `close-confirm` 93.97 s（红）、`agerc-ui-state` 31.51 s（红）、`save-graph` 90.38 s ——
这三条是"6 条占 86% 墙钟"里的三条。软件渲染是它们慢的直接原因。
