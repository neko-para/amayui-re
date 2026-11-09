# emulator-lift · 修 ComRegistry::release 的对象表：加 generation 代际（EA 复用后陈旧 Release 会打到新对象）

- id: REQ-01M6X4NFHCW1Y6YDJ4DYFA92PS
- type: bug
- status: done
- parent: REQ-01M6XVYA68DQWWMQPY795WPN0Q
- verify: apps/emulator-lift/tests/mechanism.cpp#com-generation-stale-holder
- repro: apps/emulator-lift/src/runtime/com.cpp#ComRegistry::release
- severity: S3
- tags: [emulator-lift, com, lifetime, m2]

## 判据（存在性 + 观测）

- 位置：`repo:apps/emulator-lift/src/runtime/com.cpp` 的 `ComRegistry::release`（`repro` 的锚）：按 **EA** 在对象表里查，
  而 guest 堆会**复用** EA。
- 判据：`create A → release A（refcount 到 0，表里删掉）→ create B（逼它拿到与 A 相同的 EA）→ 对旧 EA 再 Release`
  ⇒ B 的 refcount 被减（B 被提前释放 / 计数漂移）。正确行为是"陈旧 Release 打不到 B"。
- 怎么造：夹具里循环 create / release 直到新对象拿到旧 EA（guest 堆复用是常态），再对旧 EA 调一次 Release；
  判据是"B 的引用计数与后续行为不受影响"。
- 现状为什么不可复现：游戏路径里没观测到陈旧 Release ⇒ 缺陷存在、暂无观测到的分歧。

## 为什么现在不做（guest 域后果）

- 后果落在 **guest 域**：宿主不崩；表现是对象被提前析构 / refcount 漂移 ⇒ 之后某个 shim 看到半死对象，
  报"不是本 Registry 的对象"或给出错误结果 —— 归因成本远高于修它的成本。
- 严重度按"现在不触发"取 S3；一旦触发就是难归因的 guest 域错误，优先级随之上调。

## 修法边界（给将来的 agent）

- 对象表加**代际**（EA + generation），Release 时校验代际；guest 侧 ABI 不变（EA 仍是 EA，代际只活在宿主表里）。
- ⛔ 不要把"查不到"改成静默返回：现在查不到会出声，那条诊断必须留着（它是重复释放唯一可见的信号）。

## 与别的单的关系

- 同一条生命周期线三张单：**回收路径**（`REQ-01M6WTRF0X6362NK8BNK8DHHWJ`，已收口）、
  **绑定引用**（`SetRenderTarget` 的 arena 那张）、**对象表代际**（本单）。三者互不重叠。

## 收口（2026-11-09，macOS arm64 / RelWithDebInfo）

**父级裁决（判据口径）**：本单的"代际校验"只适用于**宿主侧缓存** `(EA, generation)`。
`Release` 在 guest ABI 上只带 `this`（EA），guest 拿旧 EA 发 Release **不可校验**——真机 D3D9 同样未定义；
这条是**边界**，不是未满足项。⛔ 为此改 guest ABI 不在本单（也不在白名单里）。

- 审计（可复跑）：`rg -n "com->release\(" apps/emulator-lift/src | wc -l` = 28；其中 `release(cpu, self)` 12
  ⇒ 内部裸 release 16；**修前 17**，差的那一处正是 `releaseRenderTarget` 对 owner 的 `release` → `releaseIf`。
  ⇒ 只有那一处读的是"可能陈旧的宿主缓存 EA"，其余靠缓存时 addRef 钉住。
- 修法：`ComRegistry` 宿主对象表加 `generation` / `owner_generation`（**只在宿主表**，guest ABI / `kObjectBytes` 不变）
  + `releaseIf(EA, gen)`（代际比较与减引用同一把锁）；`setRenderTarget` 的陈旧判据从
  `userOf(owner) != tex_state` 换成代际（旧判据会被"对象 EA + `'TEX1'` 状态块 EA 同时复用"骗过——实测换回旧判据 ⇒ 8 条红）。
- 守卫：`verify` 那条；端到端备选 `apps/emulator-lift/tests/d3d9_pipeline.cpp#d3d9p/stale-owner-generation-known-deviation`。
  变异：`tools/mutate-check.mjs` 两条（代际短路 ⇒ mechanism 红；判据短路 ⇒ d3d9-pipeline 红），`--match 代际` 2/2 红。
- 已知偏离（**不是**真机语义）：`GetSurfaceLevel → Release(纹理) → SetRenderTarget` 在本线被拒成 INVALIDCALL，
  因为 surface 不持父纹理；跟踪 `REQ-01M6ZZZ6PEVSFB93C6VJPKC04C`，修 04C 后该断言必须翻面。
  可达性：**未在游戏路径观测到**该顺序。
- 同根残余另开 `REQ-01M701NYF2B3YV1YFSZBMPQZ41`（Get* 借出陈旧 EA / Sprite 批次未钉住 / 流与索引假匹配）。
