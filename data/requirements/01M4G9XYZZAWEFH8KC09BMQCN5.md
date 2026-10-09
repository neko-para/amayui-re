# 前沿取证 · sub_408050（被 5 个已登记函数调用）

- id: REQ-01M4G9XYZZAWEFH8KC09BMQCN5
- type: req
- status: done
- parent: REQ-01M4G9XYQG149FB3VG5Y6FW3YY
- done_reason: 台账条目已落：subject `sub_408050/safe-vsnprintf-wrapper`（锚 = EA 0x408050/0x408073/0x408082/0x408064/0x4080A1/0x408094），并被 `pnpm tools ledger coverage` 的"全语料用得最多而没登记"榜现算出来（110 个调用方 / 205 处调用）。另一个产物是取证口径的坑：IDA 在**调用点**也复用被调方形参名写 `;` 注释且不随栈槽更新 ⇒ 判参只看 push 顺序（已登记为 `tooling/ida-callsite-param-comments-are-stale`）。行为级守卫要等模拟器实现这条语义（那时才有可观测面）——本轮的判据是"可复算的普查数 + 每条都带 EA"。
- tags: [engine, coverage]

## 为什么是它

`pnpm tools ledger coverage` 前沿：被 **5 个"已登记"函数**直接调用，而台账里没有 `bin` 锚落在它身上。
调用方横跨三条路：`sub_40ED40`（脚本装载器）、`sub_41C7C0`（`0x6 load-frame`）、`sub_422CB0`（`0x1F9 set-texture`）
⇒ 疑似**跨子系统共用的分配 / 登记 / 查表助手**（这类"共用助手"最该先登记：它错，三条路一起错）。

## 要产出的（收口判据）

1. 一条台账观察：它是什么 + 锚 = EA；
2. 三个调用点各自传什么、拿返回做什么（逐字）；
3. 守卫草案；
4. callee 里新暴露的未登记函数 ⇒ 登记进后续节点。
