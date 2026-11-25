# JS 类型门禁：scripts 22 条清零 + 收窄表达（JSDoc/TS 裁决）

- id: REQ-01M896H5YMJ5FXKTTSDEZGYJAY
- type: req
- status: done
- parent: REQ-01M7T0VJPG2S0RZ1HNWW0HH3HY
- done_reason: tsc scripts 0 条（21→0）+ pnpm typecheck 8 链绿；裁决与翻面条件写在 docs/03-emulator/js-types.md

# JS 类型门禁：scripts 21 条清零 + 收窄表达（JSDoc/TS 裁决）— 已交付

## 已交付（2026-11-25）
- `apps/emulator-lift/scripts` 的 `tsc` 从 **21 条 → 0 条**（`TS18047`×10 / `TS7006`×7 / `TS2739`×2 /
  `TS7034`+`TS7005`）。消法全是真收窄：谓词 helper `@returns {d is ConfigDomain}`、判别联合、
  逐参数 `@param`、链形状 `Chain | null`；⛔ 零 `@ts-ignore` / 零放宽类型。
- 根因归并：A `hasConfig` 回裸 boolean（3 份复制）→ 收敛进 `lib/settings.js` 的 `openConfigDomain`；
  B 散文注释缺 `@param`；C `const out = []` 在嵌套 push 里用；D `object|null` 抹掉链形状。
- `pnpm typecheck` 加第 8 条：`scripts/plugins/examples/tsconfig.json`（"只靠 d.ts 写插件"的样本进
  门禁，今天 0 条）；`plugin.d.ts` 的伪代码样例裸名 `TITLE` → `TITLE.BIN`（实现是精确相等）。
- 裁决成文：`docs/03-emulator/js-types.md` —— **继续 JSDoc / 不引入构建步骤**。

## 为什么不做 TS + 构建步骤
- JSDoc 能表达本轮全部形状（含用户点名的"`hasConfig` 为真 ⇒ domain 非空"：谓词或判别联合都行）；
  换 TS **也**表达不了属性路径断言 / `as const` / 调用点类型实参（`.js` 里） / 运行期时序。
- 构建步骤会废掉"改 `hud.js` 不用重编"（`CMakeLists.txt` 把源码树路径编进 runtime），把 Node 变成
  ctest 前置，且 8 条 CTest + 多个 tools 守卫绑死"源码树里的 `.js`"路径。
- 翻面条件（写进文档）：第二份真相 > 3 处，或同一契约在门禁红过 2 次 ⇒ 才把 `lib/**` 与 `kit.js`
  迁 TS 且**产物 `.js` 入库**（仍不引构建步骤）。

## 判据
- `node node_modules/typescript/bin/tsc -p apps/emulator-lift/scripts/tsconfig.json --noEmit` ⇒ exit 0。
- `pnpm typecheck`（8 条链）⇒ exit 0。

## 未做
- `scripts/ui/tsconfig.json` 今天没有调用者（`ui.d.ts` 由 `scripts/tsconfig.json` 覆盖）。
