# 🐞S3 EA 身份线的同根残余：sprite 早退分支裸 release / pipeWarn 配额共享且计数不入摘要 / GetDevice 无代际 / stage 覆盖与偏严 / OOM 夹具盲区

- id: REQ-01M70SHFT6RZA7RRY1R051Z2PB
- type: bug
- status: done
- parent: REQ-01M6XVYA68DQWWMQPY795WPN0Q
- verify: apps/emulator-lift/tests/d3dx9.cpp#d3dx9/sprite-early-exit-stale-texture
- repro: apps/emulator-lift/src/runtime/d3dx9_sprite.cpp#compositeBatch
- severity: S3
- done_reason: #1 sprite 早退分支：releaseEntry 现在所有出口共用 (serial, 仍是纹理) 身份校验（7 个出口都传 index），不符 ⇒ 不释放 + spriteWarn；守卫 d3dx9/sprite-early-exit-stale-texture（含正向对照），变异实测红。#2 pipeWarn：配额 key 改成 (调用点, 类别)，每类拒绝次数进摘要 pipe-warn 行；守卫 d3d9p/pipe-warn-categories，变异实测红。#3/#4/#5 父级裁决"不做"：设备 EA 纳入对象表会改设备生命周期契约且真机同样检测不到（UB）/ stage 0-7 与整笔拒绝是有意的安全选择，改成只拒该 stage 属另一条语义单 / OOM 夹具上沿要红只能断言当前取值（AGENTS §10 禁）。
- tags: [emulator-lift, d3d9, d3dx9, lifetime]

## 判据（存在性 + 观测）

`REQ-01M701NYF2B3YV1YFSZBMPQZ41` 收口时由**两轮独立复审**点名的**同根残余**（都属"按 EA / 按调用点认对象"，但本批未修）。
收在一起是为了不让它们散进对话；每条的判据是"造出那个时序 ⇒ 现在会静默用错的东西"。

| # | 位置 | 现象 | 为什么本轮没修 |
|---|---|---|---|
| 1 | `repo:apps/emulator-lift/src/runtime/d3dx9_sprite.cpp` 的 `compositeBatch` **早退分支** | 4 个早退（屏外 / 退化 / 空纹理 …）在**身份校验之前**就对该条目的纹理裸 `releaseEntry` ⇒ 越界释放后同 EA 的新纹理被减到 0 | 与 `releaseQueuedTextures` 同根，但那条只护"丢弃队列"，早退分支是**不同出口**；要改需把 serial 校验提到所有出口之前 |
| 2 | `repo:apps/emulator-lift/src/runtime/d3d9_pipeline.cpp` 的 `pipeWarn` 限流 | 配额 key 只有**调用点名** ⇒ `DrawPrimitive` 的 VB 陈旧告警可以吃光配额，纹理那条**默认级别看不见**；且拒绝计数**不进** `dumpRunSummary` | "响亮"只兑现一半：要么按 `(调用点, 类别)` 分桶计数，要么把计数落进摘要 |
| 3 | `GetDevice` 五处（纹理 / VB / IB / VD / surface） | 只对**裸 device EA** addRef、**无代际** ⇒ 设备已释放且 EA 被复用 ⇒ 把新占有者当设备 +1 | 要加"设备代际"得先裁"设备 EA 是否也进对象表";本批只做资源侧 |
| 4 | Draw 的 stage 覆盖 | 只覆盖 stage 0–7（`kMaxTextureStages`），8–15 不参与 Draw；且"任一 stage 陈旧 ⇒ **整笔**拒绝"比真机**严** | 偏严一侧是**有意**的安全选择；要不要改成"只拒那一个 stage"是语义裁决 |
| 5 | `repo:apps/emulator-lift/tests/d3dx9.cpp` 的 OOM 夹具 | 只钉**下沿**（`16u * kBatchEntryBytes` 的算式已消掉漂移，但把常量改大仍全绿：OOM 只是更早发生） | 要让它红只能把载荷钉成字面量 ⇒ `AGENTS.md` §10 禁止"断言当前取值"；**接受**这个缺口，理由写在这里 |

## 修法边界（给将来的 agent）

- #1 与 #2 最便宜、收益最直接：**先把所有出口的释放都过 serial 校验**（⛔ 不许留"某几个出口例外"），
  再让计数进摘要（`dumpRunSummary` 已有 `gpu :` 行可挂）。
- #3 需要先裁"设备 EA 的身份"：⛔ 不要在 `GetDevice` 里临时查一次 magic 就算完（那正是本批修掉的形态）。
- #4 若改成"只拒该 stage"，必须加一条守卫断言"同批其它 stage 照常绘制"，否则等于把整笔拒绝悄悄换成静默半画。
- ⛔ 不要把 5 条并成一条"清理残余"——它们的失效方式不同（丢像素 / 看不见 / 错对象 / 过严 / 夹具盲区）。

## 进展（2026-11-10）：#1 / #2 已修，#3 / #4 / #5 仍开

- **#1 已修**：`releaseEntry(ea, index)` 现在**所有出口共用**身份校验（`entryTextureIsCurrent`：serial 相符 + 仍是纹理对象），
  7 个出口（含屏外 / 退化 / 空矩形 / 无 src rect 四个早退）都传 index；不符 ⇒ **不释放** + `spriteWarn`（默认可见）。
  守卫 `repo:apps/emulator-lift/tests/d3dx9.cpp` 的 `d3dx9/sprite-early-exit-stale-texture`（含正向对照）；变异（照做校验但不据它拒绝）实测红。
- **#2 已修**：`pipeWarn` 的配额 key 改成 `(调用点#, 类别)`（类别 = vertex-stream / index / texture / other），
  **每类拒绝次数进运行摘要**的 `pipe-warn : vertex-stream=… index=… texture=… other=…` 行（仅 total≠0 时打）。
  守卫 `repo:apps/emulator-lift/tests/d3d9_pipeline.cpp` 的 `d3d9p/pipe-warn-categories`（两类各 6 次拒绝 ⇒ 摘要双计数 + 默认级别可见行 ≥10）；
  变异（类别塌成一个桶）实测红。
  ⚠ 已知取舍：计数桶与限流桶是**同一份**、进程级（不按 run 清零，因为只许动摘要落点）⇒ 单进程单次运行时等价于本轮计数。
- **#3 / #4 / #5 的父级裁决（2026-11-10，全部"不做"，各自理由）**：
  - **#3（`GetDevice` 无代际）不做**：设备是**每 Machine 一个**的宿主对象，`ComRegistry` 的代际机制覆盖资源；
    要把设备纳入对象表就得改"设备由 guest `CreateDevice` 创建 / `Release` 销毁"这条生命周期契约。
    而 `GetDevice` 拿到陈旧 EA 的前提是**guest 先释放设备、再新建一台、还拿旧句柄去用** —— 那是 guest 侧 use-after-free，
    **真机 D3D9 同样检测不到**（未定义行为）⇒ 加代际只是把 UB 变成"我们的实现比真机多一层检查"，收益不抵契约风险。
  - **#4（stage 覆盖与偏严）不做**：只覆盖 stage 0–7 是因为 8–15 不参与 Draw；"任一 stage 陈旧 ⇒ 整笔拒绝"**比真机严**是
    **有意的安全选择**（宁可少画一笔也不静默画错）。要改成"只拒该 stage"，必须同时加"同批其它 stage 照常绘制"的断言 —— 那是另一条语义单。
  - **#5（OOM 夹具上沿不可判）接受**：按 `AGENTS.md` §10，要让它红只能把载荷钉成字面量 = 断言当前取值。
- ⇒ 本单**当时能修的两条（#1 sprite 早退分支 / #2 pipeWarn 配额与计数）已修并有守卫**，其余三条**有明确裁决、不是遗漏**。

## 与别的单的关系

- 父根因已收口：`REQ-01M701NYF2B3YV1YFSZBMPQZ41`（本批四段）。
- 相邻：`REQ-01M70FP71PWDFKGQ06QE37EQJY`（响亮失败既不响也没人守 —— 那条是**信号**，本单是**身份/配额**）。
