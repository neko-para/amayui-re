# 模拟器开发（按新结构重写 + 原生输入）

- id: REQ-01M3TCB0S0BTW41KW0JBJMWHHH
- type: req
- status: open
- parent: REQ-01M3TC7BK0J9TBQKGGWQ35A3NR
- order: 20
- tags: [emulator]

## 范围
`apps/emulator`（唯一使用 TypeScript 的子项目）+ `packages/host-input`：无头驱动、帧循环、输入、渲染、存档。
旧仓 `app/amayui-emulator` 与其寄生在 `test/` 下的 10 个跨域守卫**只作参考**，重写时拆回各域。

## 判据
（待写：能起一个无头实例、能按脚本推进到指定界面并断言可观测输出。）
