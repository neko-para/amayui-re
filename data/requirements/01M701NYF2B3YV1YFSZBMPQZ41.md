# 按 EA 认对象的三处残余：Get* 借出陈旧 EA / Sprite 批次未钉住纹理 / 流与索引按状态块 EA 假匹配

- id: REQ-01M701NYF2B3YV1YFSZBMPQZ41
- type: bug
- status: done
- parent: REQ-01M6XVYA68DQWWMQPY795WPN0Q
- verify: apps/emulator-lift/tests/d3d9_pipeline.cpp#d3d9p/stale-texture-binding-in-draw
- repro: apps/emulator-lift/src/runtime/d3d9_pipeline.cpp#kPlVertexDecl
- severity: S3
- done_reason: 四段全部落地：Get* 借出 +1（GetDevice×4/GetVertexDeclaration/getTexture/Surface::GetDevice）、VB/IB/VD 补 on_zero、kPlStreams/kPlIndices 存 (EA, 代际)、sprite 批次加 serial、Set* 三处持绑定引用、kPlTextureGens 与 Draw 前的代际校验。13 条守卫逐个实测会红（verify 锚 = 像素判据 d3d9p/stale-texture-binding-in-draw）；两轮独立复审覆盖段 1-4。裁决：serial/代际只落宿主私有块（EA 从不交给 guest）⇒ 不违反「不写进 guest 可见内存」。残差 5 条已另开单（同根残余）。
- tags: [emulator-lift, d3d9, lifetime, m2]

## 判据（存在性 + 观测）

同一个根因（**按 EA 认对象 / 借出 EA 不持引用**）的三处残余，本单收在一起；各自的证据在
`REQ-01M6X4NFHCW1Y6YDJ4DYFA92PS`（已收口）的复审与审计表里：

| # | 位置 | 现象 | 为什么不是"本单顺手修" |
|---|---|---|---|
| 1 | `repo:apps/emulator-lift/src/runtime/d3d9_pipeline.cpp` 的 `GetVertexDeclaration`（`kPlVertexDecl`） | 把缓存的对象 EA 原样交回 guest；该 EA 可能已被释放并复用 ⇒ guest 拿到的是**别人的对象** | 要先裁决"Get* 是否加引用 / 是否用代际校验"，属 D3D9 语义选择 |
| 2 | `GetDevice`（`kTx` / `kVb` / `kIb` / `kVd` 四处缓存） | 同上；真机 `GetDevice` 会 **AddRef** | 会让公开 refcount 变化，牵动既有用例期望 |
| 3 | `D3DXSPRITE_DO_NOT_ADDREF_TEXTURE=1` 的批次 | 批次条目未钉住纹理 ⇒ `End`/`Flush` 可能采样到**别人**的像素 | 要修需在批次条目里存代际（**落在 guest 内存里**）或永远 addRef（改 guest 可见 refcount） |
| 4 | `kPlStreams` / `kPlIndices` | 存的是**状态块 EA**，`vertexBufferInfoOfState` 只看 magic ⇒ 状态块复用会"假匹配" | 不是 COM 对象表的事，属 D3D9 资源生命周期 |

- 共同判据："从缓存交出 / 拿回一个 EA 时，怎么知道它还是同一个对象"——修前只有 `#1/#2` 是**无校验**、
  `#3` 是**没引用**、`#4` 是**只看 magic**。
- 共同的可观测后果：guest 域拿到已释放/被复用的对象或像素，宿主不崩、结果错（归因成本高）。

## 为什么现在不触发

游戏路径里这些对象的"借出 → 使用"窗口都短于"释放 → 复用"窗口（未观测到分歧）⇒ 按"现在不触发"取 S3。
一旦时序变化就是 guest 域错误。

## 修法边界（给将来的 agent）

- 先裁决**语义**再动手：`Get*` 类方法在真机上是"借出引用"（AddRef）还是"裸借用"——按 D3D9 文档取证，
  ⛔ 不许用"本仓现在就返回裸 EA"当依据。
- `#3`：⛔ 不要把代际写进 guest 内存（那是改 guest 可见布局）；候选 = 永远 addRef（并接受 refcount 变化 + 用例期望更新）
  或宿主侧持有批次纹理的引用表。
- `#4`：判据要能区分"同一个状态块"与"同 EA 的新状态块"，候选 = 状态块里加 generation（guest 可见）或宿主侧表。
- 每处都要一条 pure 守卫：造"EA 复用后旧句柄被使用"的时序，断言新占用者不受影响 / 旧句柄被拒。

## 收口（2026-11-10）：四段落地 + 两轮独立复审

- **裁决（八点，全部按此实现）**：`GetDevice` 四处 + `GetVertexDeclaration` + `getTexture` 一律**借出 +1**（文档明确的照文档；
  文档沉默的按 Get* 家族一致）；资源**不反向持 device**（避免成环，走代际校验）；`Set*` 三处**持绑定引用**（与 `SetTexture` 同口径）；
  `D3DXSPRITE_DO_NOT_ADDREF_TEXTURE` 的 flag 语义**不改**（微软原文就是禁 Draw 的 AddRef / Flush 的 Release），只加**身份戳 + 响亮失败**；
  `VB/IB/VD` 补 `on_zero`；陈旧命中一律**响亮失败**（⛔ 不许静默采错）。
- **段 1（引用计数）**：`GetDevice` 四处 + `GetVertexDeclaration` + `getTexture` 借出 +1；`IDirect3DSurface9::GetDevice` 随后补上（段 3）。
- **段 2（身份）**：`kPlStreams` / `kPlIndices` 改存 `(对象 EA, 绑定时代际)`，Draw 侧不符 ⇒ `D3DERR_INVALIDCALL` + 告警；
  sprite 批次条目加纹理 `serial`（64→72 B），不符 ⇒ 跳过该精灵 + 告警；`Set*` 三处持绑定引用。
- **段 3（复审缺口）**：`kPlTextureGens`（16 槽，**追加在尾部**，不挪任何既有偏移）；`SetTexture/GetTexture` 验代际；
  三处 `Set*` 的"同一份绑定"改成 `(EA, 代际)`；`releaseQueuedTextures` 比 serial；`SetFVF` 两支都归还；两处失败日志升 Warn。
- **段 4（最后一处）**：Draw 在 `resolveTexture` **之前**验 `(EA, SetTexture 时代际)`，不符 ⇒ 整笔拒绝（4 个入口全覆盖）。
- **裁决（实现选型）**：sprite 的 `serial` 抄进**私有批次数组**（在 guest 堆里，但**EA 从不交给 guest**、guest 拿不到指针）⇒
  **不违反**"⛔ 不要把代际写进 guest 可见内存"；同理 `'DL01'` 私有块。若要连私有块也不许，得改宿主侧 per-state 表（改动面更大）。
- **守卫**（都能红，逐个实测过）：`d3d9/resource-getdevice-refcount` · `vertex-decl-get-refcount` · `gettexture-refcount` ·
  `resource-onzero-internal-release` · `d3d9/surface-getdevice-refcount` · `d3d9/gettexture-stale-generation` · `d3d9/setfvf-releases-decl` ·
  `d3d9/vertex-decl-stale-rebind` · `d3d9p/stale-stream-binding` · `d3d9p/binding-lifetime` · `d3dx9/sprite-donotaddref-stale-texture` ·
  `d3dx9/sprite-release-queued-stale-texture` · `d3d9p/stale-texture-binding-in-draw`（verify 锚 = 最后这条，像素判据）。
- **残差（同根，另开单跟踪，⛔ 不要在本单重复计数）**：① `compositeBatch` 的 4 个**早退分支**在 serial 校验**之前**就裸 `releaseEntry`
  （越界前提 + 屏外精灵仍会减到新占有者）；② `pipeWarn` 以调用点名作配额 key ⇒ VB 告警可吃光配额、纹理第一条默认级别看不见，
  且计数**不进**运行摘要；③ `GetDevice` 五处只对**裸 device EA** addRef、无代际（设备死 + EA 复用时会把新占有者当设备）；
  ④ Draw 只覆盖 stage 0–7（8–15 不参与 Draw），且"任一 stage 陈旧 ⇒ 整笔拒绝"比真机**严**；⑤ OOM 夹具只钉下沿（上沿不可判）。

## 与别的单的关系

- 父根因已处理的：`REQ-01M6X4MSCMMFEXYXCSK6V61QR2`（回收判据）· `REQ-01M6X4N7C0PAF6XT7EAC172V85`（RT 绑定引用）·
  `REQ-01M6X4NFHCW1Y6YDJ4DYFA92PS`（对象表代际）· `REQ-01M6ZZZ6PEVSFB93C6VJPKC04C`（surface 非 RT 寿命）。
- 本单只收"**借出/取回 EA 时没有身份校验**"这一类；⛔ 不再扩大。
