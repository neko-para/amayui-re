# emulator-lift · 修 d3d9::destroyTextureState：Machine 判据不成立时 magic 已清 ⇒ 纹理 arena 永不回收（先判后清）

- id: REQ-01M6X4MSCMMFEXYXCSK6V61QR2
- type: bug
- status: done
- parent: REQ-01M6XVYA68DQWWMQPY795WPN0Q
- verify: apps/emulator-lift/tests/d3d9.cpp#d3d9/arena-reclaim-under-other-machine
- repro: apps/emulator-lift/src/runtime/d3d9.cpp#destroyTextureState
- severity: S3
- tags: [emulator-lift, d3d9, leak, m2]

## 判据（存在性 + 观测）

- 位置：`repo:apps/emulator-lift/src/runtime/d3d9.cpp` 的 `d3d9::destroyTextureState`（`repro` 的锚就是这个名字）。
- 判据：当"取 arena 的 Machine 判据"**不成立**时，纹理状态块的 magic 已经被清掉 ⇒ 像素块**永不回收**，
  而且此后再走同一条销毁路径也进不到回收分支（magic 已是 0）。
- 可观测后果：运行摘要的 `arena` 行的 `in_use` / `surfaces` 都停在高位（该回收的那一笔没发生），
  后续 `in_use` 只能单调上升、`largest-free` 下降。
- ★ 这是**泄漏级**，不是"写坏像素"：宿主不崩、画面不错，只有 guest 堆越用越少。

## 为什么现在不做

- 游戏里只有一台 Machine、装配不会交错 ⇒ 该判据不成立的情形**在游戏路径里不触发**。
- 严重度按"现在不触发"取 S3；一旦别的装配（多 Machine / 夹具并发）进来就会变成慢性泄漏，优先级随之上调。

## 修法边界（给将来的 agent）

- 核心是"**先判 Machine 再清 magic**"（或把回收所需的引用与判据解耦）：⛔ 不要把"判据不成立"变成"静默丢资源"。
- 不要求在释放路径上加日志噪声；已有的分配失败摘要足以观测泄漏。

## 与别的单的关系

- `REQ-01M6WTRF0X6362NK8BNK8DHHWJ`（S1，已收口）修的是"根本没有回收路径"；本单是它留下的**残余**：
  回收路径有了，但判据失败时先把回收资格毁掉了。
- 同一条生命周期线上另两张：绑定引用（`kPlColorArena`）、对象表代际（`ComRegistry::release`），互不重叠。

## 收口（2026-11-09，macOS arm64 / RelWithDebInfo）

- 缺陷已复现：`Machine A` 建纹理 → 再建 `Machine B`（抢走 `g_active`）→ 用 A 的 cpu Release ⇒ 修前 d3d9 用例 4 条断言红
  （arena `surfaces` / `bytesInUse` 停高位，而 guest 堆断言绿 = 状态块已 free、像素块没入口）。
- 修法：`X86` 加**宿主侧归属** `machine`（`Machine::wire()` 装配、`~Machine` 清空）+ `machineOf(cpu)`；
  三处销毁（纹理 / 设备后缓冲 + GPU discard / 流水线 depth）改成**先取 owner 再清 magic**。
  ⛔ 未引入 live machine 注册表；guest ABI / 地址 / arena 容量未动。
- 守卫：`apps/emulator-lift/tests/d3d9.cpp#d3d9/arena-reclaim-under-other-machine`（新 23 条断言，基线在用例内捕获）；
  变异 `tools/mutate-check.mjs` 把 `machineOf` 的 owner 分支短路 ⇒ `ctest:d3d9` 红（实测）。
- 同形未修：`ddraw_surface.cpp` 的 `destroySurfaceState` 也是"先清 magic 后判 Machine" ⇒ 已另开
  `REQ-01M6ZY8SZRR3KA92ZC9QBNQ349`（各单各锚，不并进来）。
