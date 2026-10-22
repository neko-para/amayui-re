# emulator-new 平台化：核心 / 平台分层 + async/await 链路

- id: REQ-01M5GZC7B7MB4EJ7S1RH32V95A
- type: req
- status: done
- parent: REQ-01M3TCB0S0BTW41KW0JBJMWHHH
- verify: tools/test/emulator-new-isolation.test.mjs#★ 平台边界：只有 platform/** 允许 import node:*（核心的守卫是 tsconfig.core.json 的 types: []）

## 判据

- `apps/emulator-new/src/core/**` 零 `node:*` / `Buffer` / `process`；`tsconfig.core.json` 用 `types: []` 作为守卫，`pnpm typecheck` 覆盖五个工程。
- 核心只收纯数据 + `Host` 接口；`data/spec` 的读取只在 `src/platform/node/spec.ts`；`Install.open` / `readById` / `loadOpcodeTable` / `loadConfigKeys` / `analyze*` / `Machine.run` 与取件类 handler 全部 async/await。
- 纯测试仍绿（`pnpm test` 651/651），站点/隔离守卫仍绿；`node tools/mutate-check.mjs --match emulator-new` 44/44 按预期红。
- 临时在核心加 `Buffer.alloc(1)` 或 `import fs from 'node:fs'` 会让 `tsc -p apps/emulator-new/tsconfig.core.json --noEmit` 红；删掉后恢复绿。

## 范围 / 非目标

- 不新增渲染 / 音频 / 输入实现，不改变引擎语义，不引入构建步骤或新依赖。
