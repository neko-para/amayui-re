# 按 EA 认对象的三处残余：Get* 借出陈旧 EA / Sprite 批次未钉住纹理 / 流与索引按状态块 EA 假匹配

- id: REQ-01M701NYF2B3YV1YFSZBMPQZ41
- type: bug
- status: open
- parent: REQ-01M6XVYA68DQWWMQPY795WPN0Q
- repro: apps/emulator-lift/src/runtime/d3d9_pipeline.cpp#kPlVertexDecl
- severity: S3
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

- 共同判据：EOF
cat >> .tmp/reveq/eaidentity-body.md <<'EOF'
"从缓存交出 / 拿回一个 EA 时，怎么知道它还是同一个对象"——今天只有 `#1/#2` 是**无校验**、
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

## 与别的单的关系

- 父根因已处理的：`REQ-01M6X4MSCMMFEXYXCSK6V61QR2`（回收判据）· `REQ-01M6X4N7C0PAF6XT7EAC172V85`（RT 绑定引用）·
  `REQ-01M6X4NFHCW1Y6YDJ4DYFA92PS`（对象表代际）· `REQ-01M6ZZZ6PEVSFB93C6VJPKC04C`（surface 非 RT 寿命）。
- 本单只收"**借出/取回 EA 时没有身份校验**"这一类；⛔ 不再扩大。
