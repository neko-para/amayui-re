# emulator-lift · 给 D3DXCreateSprite 的资源耗尽分支补夹具（先能在夹具里造 guest 堆 OOM）

- id: REQ-01M6WW9YR4WGSBZ47DEYHYS42R
- type: req
- status: open
- parent: REQ-01M6XVYA68DQWWMQPY795WPN0Q
- tags: [emulator-lift d3dx9 test]

## 缺什么

`D3DXCreateSprite` 的**资源耗尽分支**已经修好（分配 guest 堆状态块 / 批次数组失败 ⇒ 走独立的资源耗尽
口径，⛔ 不记 unimplemented、⛔ 不 ++ 缺口计数），但**没有夹具**：`d3dx9` 现有用例只覆盖
① 正常建 sprite、② 参数 trap 三条（`d3dx9/trap-slot` / `d3dx9/trap-null-size` / `d3dx9/trap-bad-device`）。
⇒ 这条分支今天是"改坏了也没人知道"。

## 判据

1. **先能造 guest 堆 OOM，且不改机器**：`repo:apps/emulator-lift/tests/d3dx9.cpp` 的夹具用的是**远小于
   真机**的 heap 尺寸（夹具自己 init 的那个）。用它把 heap 占到"再放 sprite 状态块 + 批次数组就不够"，
   ⛔ 不要为了造 OOM 去改机器默认 heap / arena 尺寸（那会改 guest 可见地址）。
2. 占满后调一次 `D3DXCreateSprite`，**四条都断言**（缺一条都不算）：
   - 出参 `ppSprite` 仍是 NULL（真机失败时也保证出参为 NULL）；
   - 停机口径是**独立**的资源耗尽（`stop_why` 是那个独立标签，**不是** unimplemented）；
   - **缺口计数没有增加**（`unimplemented_calls` 前后相等）—— 这是"误诊回归"的唯一机械判据；
   - 把为造 OOM 占的块释放后 heap 回到基线（`bytesInUse` 逐字节相等）⇒ 失败路径没留下半建状态块 / 批次数组。
3. **变异自检**：把资源耗尽那条早退改回 unimplemented 口径（或短路掉）⇒ 本用例必须红。

## 为什么现在不做

造 OOM 需要**夹具级堆占满**这套基建，那是另一条活（本轮修复只到"分支口径正确"）。硬写最可能的形态
是"把机器 heap 调小"或"伪造前提直接造状态" —— 两者都会让用例证明的不是机器的行为。

## 现在绕到了哪里

- 分支的正确性目前靠**同口径的兄弟路径**：纹理分配失败那条有单元 + 端到端实测（见
  `repo:apps/emulator-lift/tests/d3d9.cpp` 的纹理用例与 `repo:apps/emulator-lift/tests/option_roundtrip.cpp`），
  sprite 只是复用同一套结构化出参协议。
- 真出 OOM 时 stderr 有独立标题、运行摘要 `detail` 会带标签 —— 但那是**人工看日志**，
  没有任何 CTest 会在它坏掉时红。

## 边界

- ⛔ 不改 `D3DXCreateSprite` 的失败语义（真机返回 `E_OUTOFMEMORY`；本线为什么改成硬停，理由在
  `REQ-01M6WTRF0X6362NK8BNK8DHHWJ` 的实测里，⛔ 不要在这里重新论证）。
- ⛔ 不把"夹具能造 OOM"做成产品参数（那是测试侧的事，不给用户一个能调小 heap 的开关）。
