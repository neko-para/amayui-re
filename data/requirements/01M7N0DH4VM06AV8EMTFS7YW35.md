# AOT feeder 又撞上令牌头阻塞：最坏一刀 1.0 s / 2 次 underrun（历史 1.3–1.7 s 全 shimtok 的重开单）

- id: REQ-01M7N0DH4VM06AV8EMTFS7YW35
- type: bug
- status: dropped
- parent: REQ-01M7JHSDH16HCVEV4BA6NKSHN2
- repro: apps/emulator-lift/src/runtime/shims.cpp#t_shim_token_wait_ns
- severity: S2
- dropped_reason: 2026-11-18 复测（当前 HEAD 00af635，mac arm64）：转场 90 s run 两跑 underruns=0、fill_max_ms=18.99/351.85、fill_wait_ms=0.00/9.00 ⇒ **令牌等待不是当前瓶颈**。原先那 1.0 s 一刀是缺省 caps=0 时"渲染落在 CPU"的产物（软件模糊 6 帧），P4 把 caps 固定 2.0 后不再出现。若将来在更重场景复现（fill_max 逼近 500 ms 且 fill_wait_ms 占大头），按本单正文 M0/M1 重新立案。
- tags: [emulator-lift, audio, token, performance]

## 一句话

真 feeder 落地后，**解释器时代那条"feeder 在 shim 边界被主线程挡在令牌外"的尖峰又回来了**：
AOT 转场 90 s run 里最坏 **1.0 s 一刀**（`fill_max_ms=1016.04`、`underruns=2`）。
令牌是**容器锁**（一个 token 保护所有 guest 可见容器）⇒ 音频 feeder 与渲染/业务主线程在 shim 边界**无意义地互相阻塞**。

## 依据

- 历史（解释器时代，已 dropped 的 `REQ-01M75VZXV4SDMVVM1Z72G009XJ`）：
  "feeder 在 shim 边界被主线程长时间挡在令牌外（**单刀 1.3–1.7 s 全是 shimtok**）⇒ 静音晚到 ⇒ 尾声重播 2–3 次"。
  它的 `dropped_reason` 明确写了："AOT 今天根本没有 feeder ⇒ 无从复现。**AOT 接音频后若再现尾声重播，按'与暂停/令牌的配合'重新立单**。"
  ⇒ 现在 AOT **有** feeder，且读数形状一致 ⇒ 按它的原话重开（不是复活）。
- 现状读数（`docs/03-emulator/audio.md` §4）：`fills=186 / 90 s`（平均 484 ms/刀，无余量）、`fill_max_ms=1016.04`、`underruns=2`。
- 架构口径（`guest_token.h` 头注）：**不是执行锁**，是"容器临界区"；不变式 = 任一时刻最多一个宿主线程持令牌、
  guest 可见共享容器（GuestHeap / HandleTable / Vfs / ComRegistry / SurfaceArena / 内存账本）只在令牌里访问。
  ⇒ 因为**这些容器本身是全局的**，一把锁就够；代价就是**跨子系统头阻塞**（真 Win32 是 per-heap / per-object 内部锁）。

## 要做什么（先测量，再决定 M0 / M1 / 分锁）

1. **先量**：最坏那刀里 `t_shim_token_wait_ns` / `t_shim_token_wait_count` 占多少（`shims.cpp:548` 的只读计量）。
   - 占大头 ⇒ 本单成立（与解释器时代同一条）。
   - 占很小 ⇒ 关掉本单，转"decode 本身重"（`REQ-01M7N0G…`）。
   ★ Phase-0 的读数工具（持有占空 / 最长持有 / 等待直方图 / 每 shim max_ns）来自已 done 的
   `REQ-01M75WWEDC20GEH7QVSHPHH4GP`，直接复用，⛔ 不要重造。
2. **两个候选修法**（历史计划 `REQ-01M75WWFGT8ZEBGEPENQHSYTB8`，M0 已部分落地 = `REQ-01M7614GMRFD7DNE4DHNBEWZHW` Present 内 ~250 ms 残留已修）：
   - **M0（推荐先做）**：把"阻塞等待 / 宿主专属重活"（Present、纹理上传、文件 I/O、大 memcpy）移出令牌作用域
     ⇒ 不需要拆容器，但要守 `Yield` 后重校验的纪律（`guest-token/yield-revalidate`）。
   - **M1**：令牌作用域缩到**容器区域**（只包住容器访问本身）。
   - （更远）按容器分锁（heap / handles / COM / surface arena）+ 固定锁序 —— 大改，latch 之后再谈。
3. **判据**：120 s 转场 run `underruns=0`；或 `fill_max_ms < 500`；且 `t_shim_token_wait_ns` 不再是 fill 里的大头。

## 未做

⛔ 不在本单拆容器分锁（那是 M1 之后）；⛔ 不许用 `LIFT_TOKEN_DIAG_SKIP_LOCK` 当修法（它是诊断臂，只在单线程工况）。
