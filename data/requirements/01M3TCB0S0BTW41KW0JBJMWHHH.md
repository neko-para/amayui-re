# 模拟器（apps/emulator-new：只读 data/spec 的参考实现）

- id: REQ-01M3TCB0S0BTW41KW0JBJMWHHH
- type: req
- status: open
- parent: REQ-01M3TC7BK0J9TBQKGGWQ35A3NR
- order: 20
- tags: [emulator]

## 范围

`apps/emulator-new`：**只读 `data/spec`** 的参考实现（真实入口 + 无头执行；不做渲染 / 音频 / 输入）。
旧实现 `apps/emulator`（含 `packages/host-input`）**已废弃**、只作架构参考 —— 原「批 M4 · 模拟器与原生输入」
及其子单（M4-1 / M4-2 / M4-1b / M4-1b-1 / 拆跨域守卫 / 状态分区 / 能力普查）已全部 drop，不再是工作项。

★ 语言与工具链口径**不在本单复述**（真源 `AGENTS.md` §3）：`.ts` 无构建步骤（Node 原生剥壳直接跑，`tsc` 只做 `noEmit`）。

## 判据

（待写实：从安装目录的真实入口跑通到指定界面；**未实现即抛错**，不 no-op。）

## 子单（当前活）

- 帧模型与引擎「40 槽定长数组 + 当前帧号」不等价（守卫全在 LIFO 子集内）：`REQ-01M5D251ZJTZA4S6SX170RAXD2`
- spec 修订后的实现 / 守卫对齐（0x320 地址读 / 窗收尾 / 0x9 复位 / 0x6 argc）：`REQ-01M5ED27BF1T6NSEJ05GT2XFW3`
