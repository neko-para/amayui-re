# emulator-cxx 原生宿主（C++）：按 spec 重写的实现线

- id: REQ-01M5SZ3M7GCR0M23RSYBD7GEA5
- type: req
- status: open
- parent: REQ-01M3TC7BK0J9TBQKGGWQ35A3NR

## 判据

- 每一批 emulator-cxx 的实现都要能挂上一个**会红**的守卫：C++ 侧是 `apps/emulator-cxx/tests/core_selftest.cpp` 的用例（ctest 跑），
  分派层是 `tools/test/emulator-cxx-dispatch.test.mjs`（`pnpm test` 跑，带 `tools/mutate-check.mjs` 的一条变异）。
- `data/spec` 是唯一真源：C++ 只消费规范结论，⛔ 不把 emulator-cxx 的实现细节写回 spec，
  也⛔ 不拿 `apps/emulator-new` 当语义真源（它只是旁证；两者冲突时按 spec 与语料裁决）。

## 口径

- 这是**原生宿主**（SDL3 / SDL_GPU）那条实现线，与 Electron 那条（`apps/emulator-new`）互不依赖。
- 范围按**脚本足迹**切（`--footprint` 动态足迹 + 脚本静态足迹），不按章节、不按子系统名切；
  audio / text-subsystem / live2d 三个模块暂不在范围内（各自的 handler 是显式占位）。
- 已知的"看起来像卡死"：门字第 27 / 30 位没有服务分支（规范如此，第 30 位「启用后门恒关」）⇒ `run()` 会到 step 上限。

## 下一步

- 本节点的子节点各自是一个可验收的批次；没有子节点的"要不要做"不进这棵树。
