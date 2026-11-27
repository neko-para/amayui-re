# 插件触发模型 v2：AGE VM 指令点 + TypeScript 插件（分析/设计）

- id: REQ-01M8EKFAP4QJY9G12K7KB9YEQB
- type: req
- status: open
- parent: REQ-01M7JHSFB2SSBBNSJ82YDED8M1
- tags: [emulator-lift, plugin, qjs, typescript, design]

## 一句话

把插件触发从 input-plan 遗留的 Win32 读点模型，重构为 AGE VM 指令点模型；统一同步/异步 JS 回调，缓存 ctx + getter 查询，插件直接用 TypeScript（quickts E1）。

## 结论

- 异步触发 = `TriggerRunner` 在 11 个 Win32 投递点 poll（9 个语义读点 + `SetCursorPos` + `FileRead`）；可跑任意 JS，但位置触发被读点量化。
- 同步触发 = `instr_hook` 在 3 个脚本派发 EA 的每条脚本指令前；只有 `at/capture/patch`，没有 JS。
- 用户方向：插件改的是 AGE VM，不是 x86；触发应在 VM 指令点 / AGE input opcode，Win32 读点只留输入投递。
- 性能不是“指令点 poll”的问题：1 条 hook +0.62%（上界 1.3%），cheap key 6–8 ns 比今天读点 `sampleNow` 160 ns 更便宜；贵的是每条指令跑 JS（空回调 ≈5 µs）。
- 渲染只有 `render` 有 builder；指令点没有 `draw`；UI 状态只在 render 改/画；逻辑只改数据。
- TS：quickts E1 移植到 quickjs-ng 已跑通（33 hunk/+187/−6，JS 字节码 16/16 恒等，examples 9/9，`__text` +8.6 KB）；仍需 `tsc --erasableSyntaxOnly` 门禁。

## 子单

1. `REQ-01M8EKFHDFX7DP4FCWVA7MPQET` 现状：读点异步 vs 指令点同步（能力/性能/渲染/缺陷）——已收口，作为事实底稿。
2. `REQ-01M8EKFS5HSQFP93T19JDD50ZD` 新 JS API：`RunCtx`/`Tick` · `at/once/task` · timeout/日志/UI 分层。
3. `REQ-01M8EKFSVE4RF8WZW60C5QHV59` TypeScript：quickts E1 移植 + `erasableSyntaxOnly` + 实施分片。

## 不做 / 边界

- 不做逻辑 timer（固定时间触发不知道 guest 位置）；timeout 只做失败截止。
- 不做嵌套 Run；同一 endpoint 一个 active Run，dispatch busy。
- 不做 enum/参数属性/JSX/装饰器（v1 由 `erasableSyntaxOnly` 排除）；要时再移植 E2。
- 不保留旧 `start`/`onTrigger` 返回 set 的状态机；示例直接迁移。
- 不把 Win32 读点作为插件触发源；`AtReadPoint` 仅 deprecated 兼容。

## 怎么查

- 合同：`docs/03-emulator/plugin-runtime.md`
- 引擎：`apps/emulator-lift/third_party/quickjs-ng`；宿主：`apps/emulator-lift/src/ui/script_quickjs.cpp`
- 进度：`pnpm tools requirements plan`
