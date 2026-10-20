# apps/emulator-new：只读 data/spec 的参考实现（无渲染/音频/输入）

- id: REQ-01M5AJX0EDG77VSCDG7D9Z7TZS
- type: req
- status: done
- parent: REQ-01M5AJWPM4DXWG3XR6W7JR6XHD
- order: 50
- verify: tools/test/emulator-new-isolation.test.mjs#★ 只读 data/spec
- tags: [emulator spec-driven]

## 目标

新建 `apps/emulator-new`：**只读 `data/spec`** 的参考实现（不含渲染 / 音频 / 输入），用于验证 spec 准不准、够不够。
不复制 `apps/emulator` 的代码；只允许读它的架构文档。

## 判据

1. 代码里没有 `@amayui/age-format` / `data/shape` / `data/ledger` / `corpus` 的 import（守卫钉住）。
2. 能从 `data/spec` 读指令表与类型标签，把一份真实脚本跑到"未实现"并**报缺口**（未实现即抛错，不 no-op）。
3. 单测在无窗口、无原生输入下可跑。

## 非目标

不实现 render / audio / input；不追求跑完整游戏（后续批次）；不与旧模拟器共享代码。
