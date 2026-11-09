# 修纹理 surface 的非 RT 寿命：surface 只存裸 user EA、不持纹理 ⇒ 纹理先释放后 surface 仍可被用

- id: REQ-01M6ZZZ6PEVSFB93C6VJPKC04C
- type: bug
- status: open
- parent: REQ-01M6XVYA68DQWWMQPY795WPN0Q
- repro: apps/emulator-lift/src/shims/d3dx9_surface.h#createD3dxSurfaceObject
- severity: S3
- tags: [emulator-lift, d3dx9, lifetime, m2]

## 判据（存在性 + 观测）

- 现象链（代码证据）：`repo:apps/emulator-lift/src/shims/d3dx9_surface.h` 的 `createD3dxSurfaceObject(cpu, user, owner)`
  建出的 surface 对象 `on_zero = nullptr`（surface **不拥有**像素块与 `'TEX1'` 状态块），`user` 只是**裸 EA**；
  像素块 + 状态块的唯一回收入口是**纹理对象** refcount 到 0 时的 `destroyTextureState`。
- ⇒ 若 guest 释放纹理、但**仍持有该 surface**（`GetSurfaceLevel` 拿到的那个），再用它做
  `GetDesc` / `LockRect` / 用它的 `user` 解状态块：状态块已被 free、EA 可能已被堆复用 ⇒ 读到别人的数据或野值。
- 与已收口的 `REQ-01M6X4N7C0PAF6XT7EAC172V85` 的区别：那条只保证"**被绑定为 RT 期间**像素块不被回收"
  （绑定路径对 owner 成对 addRef）；本单是**非 RT** 的 surface 寿命：surface 不持有纹理，纹理走了它还在。

## 为什么现在不触发

游戏路径里 surface 的使用窗口都在纹理活着的时候（未观测到分歧）⇒ 按"现在不触发"取 S3。
一旦有"先 Release 纹理、再用 surface"的顺序就是 guest 域读已释放内存（宿主不崩、结果错）。

## 修法边界（给将来的 agent）

- 两条候选，二选一并在实现前裁决：
  A. 让 surface **持有**纹理对象（`GetSurfaceLevel` 建 surface 时对父纹理 addRef，surface 的 `on_zero`
     成对 release）—— 语义上最接近真机；代价是 `tests/d3dx9.cpp` 里既有 Release 返回值期望要跟着改。
  B. surface 自带状态块 / 像素块的所有权（复制或转移），不与纹理共享 —— 代价是内存翻倍 / 语义更远。
- ⛔ 不许把 `user` 改成"查不到就 silent 0"：那会把"读已释放"伪装成"没有扩展块"。
- 守卫：`repo:apps/emulator-lift/tests/d3dx9.cpp` 造"纹理 Release → 仍持 surface → `LockRect`/`GetDesc`"，
  断言 surface 仍可用且 `user` 指向的块仍归它（A 方案下 = 纹理没被回收）；变异把配对 addRef 短路 ⇒ 红。

## 与别的单的关系

- 同一条生命周期线：回收路径（`REQ-01M6WTRF0X6362NK8BNK8DHHWJ`，已收口）· 绑定引用
  （`REQ-01M6X4N7C0PAF6XT7EAC172V85`，已收口）· 对象表代际（`REQ-01M6X4NFHCW1Y6YDJ4DYFA92PS`，未收口）· **本单**。
