# 修 ddraw::destroySurfaceState：同 d3d9 那族——先清 magic 后判 Machine ⇒ 表面 arena 永不回收

- id: REQ-01M6ZY8SZRR3KA92ZC9QBNQ349
- type: bug
- status: open
- parent: REQ-01M6XVYA68DQWWMQPY795WPN0Q
- repro: apps/emulator-lift/src/runtime/ddraw_surface.cpp#destroySurfaceState
- severity: S3
- tags: [emulator-lift, ddraw, leak, m2]

## 判据（存在性 + 观测）

- 位置：`repo:apps/emulator-lift/src/runtime/ddraw_surface.cpp` 的 `destroySurfaceState`（`repro` 的锚就是这个名字）。
- 形状与 `REQ-01M6X4MSCMMFEXYXCSK6V61QR2`（d3d9 纹理那条，已收口）同族：先 `wr32(state + kSfMagic, 0)`、释放状态块，
  **之后**才用 `activeMachine()` 当"该不该回收 arena"的判据：
  `if (Machine* m = activeMachine(); m && &m->cpu() == &cpu) { if (arena_ea != 0) m->surfaces().destroy(arena_ea); }`
  ⇒ 判据不成立时 magic 已清、链已摘 ⇒ 像素块**永久没有回收入口**（泄漏级：宿主不崩、画面不错，guest 堆越用越少）。
- 什么时候判据不成立：`g_active` 被另一台 Machine 抢走（`Machine::init()` 写、`~Machine` 清）、或为 `nullptr`。

## 为什么现在不触发

游戏路径只有一台 Machine、装配不交错 ⇒ 现状不可观测。严重度按"现在不触发"取 S3；
一旦多 Machine / 夹具并发进来就变成慢性泄漏。

## 修法边界（给将来的 agent）

- 与 d3d9 那条同一修法：用**宿主侧归属**（`repo:apps/emulator-lift/src/runtime/x86.h` 的 `X86::machine` +
  `machineOf()`）取 owner，**先取 owner 再清 magic**；⛔ 不许把"判据不成立"变成静默丢资源。
- 链摘除 / `kDevPrimary` 归零是 guest 可见语义，照旧要发生；只有 arena 回收要换成 owner 判据。
- 守卫：`repo:apps/emulator-lift/tests/ddraw.cpp` 造"Machine A 建 surface → Machine B 抢 `g_active` →
  用 A 的 cpu 销毁"，断言 arena `surfaces` / `bytesInUse` 与设备链都回基线；变异把 owner 分支短路 ⇒ 该 CTest 红。
- 变异条目：`repo:tools/mutate-check.mjs` 一条（guard `ctest:ddraw`）。

## 与别的单的关系

- 同族：`REQ-01M6X4MSCMMFEXYXCSK6V61QR2`（d3d9 纹理，已收口，修法可直接照抄）。
- ⛔ 不与 `SetRenderTarget` 绑定引用 / `ComRegistry` 代际混在一起：那两条是"对象表与像素块的生命周期"，本单只是"回收判据"。
