# 前沿取证 · sub_4E73EB（被 12 个已登记函数调用）

- id: REQ-01M4G9XYVNEE8X6K2FKVVKMYJH
- type: req
- status: done
- parent: REQ-01M4G9XYQG149FB3VG5Y6FW3YY
- done_reason: 取证结论**推翻了这条前沿项本身**：sub_4E73EB 不是引擎函数，是 MSVC /GS 栈 cookie 校验桩（15 字节，1144 个调用点都是各函数的 /GS 尾声）。台账已落 subject `crt+0x4E73EB/msvc-gs-security-cookie-check`（6 个锚）。副产品更要紧：这条直接催生了 tools/lib/coverage.mjs 的**库代码分类**（thin-forwarder-to-library / calls-stl-internal），把 CRT/STL 从分桶与前沿里摘出去（可见、可复核、带理由）。
- tags: [engine, coverage]

## 为什么是它

`pnpm tools ledger coverage` 前沿**第二名**：被 **12 个"已登记"函数**直接调用，而台账里没有 `bin` 锚落在它身上。
调用方横跨三条路 —— `sub_42AEA0`（取址原语）、`sub_40ED40`（脚本装载器）、`sub_422E70`（`0x1FB draw-texture`）
⇒ 处在"取址 / 装载 / 绘制"的交叉点上。

## 要产出的（收口判据）

1. 一条台账观察：**它是什么**（只用可核事实：它碰的字段 EA、调用的符号、静态串）+ 锚 = EA；
2. 它在三条调用路里分别被用来做什么（至少把**调用点**的逐字列出来）；
3. 守卫草案（能机械判的）；
4. callee 里新暴露的未登记函数 ⇒ 登记进后续节点。

★ ⛔ 不许用"名字像不像"定名。拿不到就写"缺哪个观察"。
