# 音频双 feed 路径的清理与口径：GetMessage 喂料从未存在；删只为双路径的死同步（latch/had_token）；D 只服务 virtual/确定性

- id: REQ-01M775FA9TGEX1X1CQW03AKBHA
- type: req
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- done_reason: 双 feed 口径订正 + 删死同步（latch/had_token 净−26 行）+ 文档标 GetMessage 喂料未实现；D 保留（virtual/确定性/降级），退役 D 另单
- tags: [emulator-lift, audio, cleanup]

## 一句话
审计结论：**"GetMessage 里异步喂料"这条实现今天不存在、历史提交里也从未存在过**（236 个提交 + 全部 unreachable commit 的
`setHostEventPump` lambda 只有 `pumpHostEvents` / `tickInputScript` / `tickInteraction`，零 audio）。
今天真正并存的两条是 **C（真线程 `feederThreadMain`/`feederEvent`）** 与 **D（块边界 `pump()`）**；
用户记忆里的"两个 feed 实现"= 这两条。D 不能直接删（它还是 virtual/无 mixer 档唯一的设备游标+混音驱动器、
`--audio-dump` 确定性、`audio-device` 13 个测试函数/17 处 `pump()`、以及 C 的 `setupFeederContext` 失败降级）。
本单只做**清理死同步 + 把口径写死**；"退役 D"是另一张单（见 `REQ-01M75WWF...` 的裁决）。

## 已核事实（`.tmp/audio-token/feed-paths-audit.md`）
- C：`resumeFeeder`→`startFeederThread`(`audio.cpp:1552`)→`feederThreadMain`(:1571)→`feederEvent`(:1590)→`runFeederGuest`(:1863)；
  仅 `--audio sdl` + real-time（窗口自动 real-time）。
- D：`AudioEngine::pump()`(`audio.cpp:1164-1389`)；生产唯一调用点 `machine.cpp:1447`；测试 17 处。
  两者是**字面意义的代码重复**（各写一遍"12 段 native + 1 段 guest"）。
- `pump()` 的 `feeder_thread_started_` 单向闩（`audio.h:368-376`）与锁内 `had_token` 断言（`audio.cpp:1199-1206`）今天**不可达**：
  `startFeederThread` 只有 `setupFeederContext` 失败会回退（在闩/线程之前），`std::thread` 构造无 catch ⇒ 抛异常即 terminate。
  ⇒ 这两处是"为双路径而存在的死同步"。
- 文档滞后：`apps/emulator-lift/docs/thread-model-feasibility.md:416` 仍把 GetMessageA 泵列为"**可选**触发点"；
  `docs/audio-implementation.md` §0.0 只写 D 的"SE 延迟 600 ms + 断裂"，没写"D 只服务 virtual/确定性"。

## 要做
1. `audio.h` / `audio.cpp`：删除 `feeder_thread_started_` 单向闩 + `pump()` 的 `had_token` 断言位（连同"为双路径"的注释），
   只留 `thread_mode_` 早退；`pump()` 旁写清"本函数只在 `thread_mode_ == false` 可达；C 的失败路径不会回退到 D"。
   ⛔ 不动 `thread_mode_` 块、⛔ 不动 `GuestToken::Guard`（测试/块边界仍需要）、⛔ 不动 `deferred` 分支与四连早退。
2. `apps/emulator-lift/docs/thread-model-feasibility.md`：把"GetMessageA 泵可选触发点"标成**未实现 / 从未存在**
   （引用审计：`main.cpp:1104` 的 lambda 零 audio），并写"今天只有 C 与 D 两条，D = virtual/确定性/降级"。
3. `apps/emulator-lift/docs/audio-implementation.md` §0.0：补一句"D 保留给 virtual / 无 mixer / 确定性 dump / C 起不来的降级；
   窗口 + real-time 永远是 C"；§"怎么读"补 `fill_wait_ms` / `fill_wait_max_ms`（与 `REQ-01M75RD3...` 的读数口径一致）。
4. 不删 D 的代码、不改 CLI 语义（`--audio sdl` 无 real-time 仍走 D）。

## 判据
- `rg feeder_thread_started_` = 0；`rg had_token` 只剩不涉及该断言的注释（或 0）。
- `ctest -R audio-device` 绿；`ctest -E overlay-window-smoke` 全绿；`pnpm test` / `pnpm verify` 绿。
- 文档不再声称 GetMessage 喂料存在；D 的口径写死。
- ⛔ 不删 D、不改 `kFillWaitTimeoutMs`、不动真线程的令牌/补偿机制。

## 收口（2026-11-12）
- **口径订正（以代码为准）**：C 的 `setupFeederContext` 失败**确实回退到 D**（`thread_mode_=false` + "退回主线程泵"），
  但失败发生在**建线程之前** ⇒ 要防的状态是"feeder 线程在跑而 `pump()` 走 D"，今天不可达。
  另：`--present window --audio virtual` 仍是 **D**（`thread_mode_ = Sdl && realTime`）；文档写的是
  "窗口 + `--audio sdl` 永远是 C；窗口 + `--audio virtual` 仍是 D"。
- **已删**：`feeder_thread_started_` 单向闩（`audio.h` 9 行）+ `startFeederThread` 置位（4 行）+ `pump()` 的 `had_token`
  断言位（24 行），净 −26 行；`rg` 在 `apps/ docs/ tools/` 为 0。`pump()` 旁写清"只在 `thread_mode_ == false` 可达"
  与"将来加 `std::thread` catch / feeder restart 时先回来重加硬门"。
- **文档**：`thread-model-feasibility.md:416` 把 GetMessageA 泵标成"未实现 / 从未存在"（审计：`main.cpp:1104` 的 lambda
  零 audio；236 提交 + unreachable 均无）；`audio-implementation.md` §0.0 写死 D 只服务 virtual / 无 mixer /
  确定性 dump / C 起不来的降级。
- 门禁：build 绿 · `audio-device` 1/1 · ctest 84/84（5 skip）· `pnpm test` 546 · `pnpm verify` 460 · requirements 6/6 ·
  `git diff --check` 干净。**D 未删**（退役 D 是另一张单：要重写 13 个确定性夹具）。
