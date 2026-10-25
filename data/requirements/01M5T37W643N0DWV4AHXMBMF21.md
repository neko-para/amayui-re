# emulator-cxx · 剩余 7 条：存档族

- id: REQ-01M5T37W643N0DWV4AHXMBMF21
- type: req
- status: done
- parent: REQ-01M5SZ3M7GCR0M23RSYBD7GEA5
- done_reason: 存档族 7 条落完 ⇒ 排除 audio / text-subsystem / live2d 之后**剩余缺失指令 = 0**（对照 apps/emulator-new 的 HANDLERS 表机械核过）。守卫：core_selftest 的 check_save_ops（内存 FakeSaveHost）+ 分派表守卫；门禁 ctest 5/5、pnpm test 1071/0、test:mutation 363/363。

## 还剩 7 条（排除 audio / text-subsystem / live2d）

**存档族**：`0x19E`（写）· `0x1A0`（读文本）· `0x1A1`（读二进制）· `0x1AB`（删）· `0x1AC`（拷）· `0x1AE`（缩略图写）· `0x1AF`（缩略图读）。

- 依赖：`SaveHost`（**宿主注入**的 IO 边界：写 / 读 / 删 / 拷 / 缩略图），核心只定义接口；⛔ 核心不许直接碰 `std::fstream`。
- 参考实现：`apps/emulator-new/src/core/save-codec.ts` + `save.ts`；缩略图是 STH（BMP 容器）。
- 待定（规范未取证）：槽数、文件名编码、缩略图格式细节 —— 这些必须按 spec 的残差处理，不许编。

## 收口判据

- `core_selftest` 每条至少一个**会红**的用例（用内存宿主最稳）。
- `tools/test/emulator-cxx-dispatch.test.mjs`；`ctest` 5/5；`pnpm test` 不红。
