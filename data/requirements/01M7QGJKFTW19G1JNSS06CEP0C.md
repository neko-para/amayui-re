# 🐞S3 QuickJS 沙箱：generator / async 构造器仍可动态编译（只删了 Function.prototype.constructor）

- id: REQ-01M7QGJKFTW19G1JNSS06CEP0C
- type: bug
- status: open
- parent: REQ-01M7PE0D9MJA4REXQ1YJFVPHGF
- repro: apps/emulator-lift/src/ui/script_quickjs.cpp#installSandbox
- severity: S3
- tags: [emulator-lift, quickjs, sandbox]

## 判据（存在性 + 观测）

- 现状：沙箱删了 `globalThis.Function` / `Function.prototype.constructor` / `globalThis.eval` / `Math.random`（`src/ui/script_quickjs.cpp:202-260` 的 `installSandbox`），但 **generator 的 constructor 没删**：
  `(function*(){}).constructor('yield 1')` 实测仍能动态编译并返回 `[object Generator]`（探针见 `.tmp/overlay-profile/module-probe/RESULTS.txt`；`gf=compiled:[object Generator]`）。
- 同族未删：`(async function(){}).constructor`（AsyncFunction）与 `(async function*(){}).constructor`（AsyncGeneratorFunction）；QuickJS 里挂在各自 class proto 上（`quickjs.c:51499` / `:51547`），不是 `Function.prototype`。
- 影响：用户脚本可绕过"没有动态编译"的沙箱合同。目前探针没有拿到宿主指针，但合同已破；一旦以后给 `ui`/`game` 加更多宿主能力，这就是一条绕过静态检查的入口。

## 修法（待做）

1. `installSandbox` 里用 `JS_GetPrototype` 拿到 generator / async / async-generator 的 class proto，`JS_DeleteProperty(proto, "constructor")`；若引入 Promise intrinsic（module 需要），同时 `globalThis.Promise = undefined`（模块内部仍可用）。
2. 守卫：`tests/ui_script_quickjs.cpp` 加 case，断言四种 constructor 都不可调用（`typeof !== "function"` 或调用抛 TypeError）；已有 `Function` / `eval` 断言保留。
3. 与 `REQ-01M7QF01A00F9946DX0GPH23JQ` 的 module 改动同批或紧邻；module 用例（`ui-script-modules`）必须仍绿。

## 不做 / 边界

- 不追求"语言里完全没有 async / generator"（module 本身是 async，generator 是语言特性）；只保证**没有动态编译入口**。
- 不把 `Promise` 从语言里拿掉（module 硬前提），只删全局名字。

## 复现

- 代码锚：`apps/emulator-lift/src/ui/script_quickjs.cpp#installSandbox`。
- 探针：`.tmp/overlay-profile/module-probe/`（本地只读，不入库；`RESULTS.txt` 有 `gf=compiled` 与 `af=TypeError` 的对照）。
