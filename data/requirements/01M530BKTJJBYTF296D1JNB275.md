# 批 S1d · 跳转/调用族 handler 的 Frame 与 Engine 字段细化（control-flow-handlers.json）

- id: REQ-01M530BKTJJBYTF296D1JNB275
- type: req
- status: done
- parent: REQ-01M52F3ASMGWQXX64JSA18MP2N
- order: 44
- verify: tools/test/shape-claims.assets.test.mjs#★ 入库的 shape 模型（data/shape/）逐族全绿 + 守恒闭合
- tags: [engine shape]

## 目标

为**指令跳转 / 调用相关指令的 handler 族**（`call-script` / `ret` / `local-ret` / `load-frame` / `call-frame` /
`jmp` / `call` / `exit` …）产出载体表，并借此**细化 `Frame`（记录类型）与 `Engine` 的字段**。

## 为什么值得单独做

* `types.json` 目前的成员几乎全部来自数值族：数值族**只碰**记录数组的**一个**元素内偏移 ⇒
  它对"记录内部长什么样"几乎说不出话。跳转族才碰**帧内多个偏移**与**Engine 级的每帧数组**。
* 这一族同时给出"记录数组是**帧栈**"这一读法的形状证据（`ret` 里出现"索引−1"的写法），
  与同级的**原点约定**那张单互为佐证。

## 判据

* `control-flow-handlers.json` + 中心 `types.json` 的增量：`pnpm tools shape claims data/shape/control-flow-handlers.json`
  **退出码 0**；
* 每条新增成员都带 **witness EA**，且抽验过一遍 `pnpm tools shape verify <EA>`（独立复核器，不信模型转述）；
* 判定不了的（负原点 / 二维索引 / `ds:` 符号 / 无法检查的载体）**逐类如实报**，⛔ 不许为变绿编成员；
* 无对象根寄存器的 handler（`.c` 约定里查不到）**不许**建载体冒充覆盖。
