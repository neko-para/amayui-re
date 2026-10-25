# emulator-cxx · 按住派发族 5 条（0xFC / 0x102 / 0x103 / 0x104 / 0x105）

- id: REQ-01M5T4D6W2QMC4RF87ZGFXHD5K
- type: req
- status: open
- parent: REQ-01M5SZ3M7GCR0M23RSYBD7GEA5
- verify: tools/test/emulator-cxx-dispatch.test.mjs#★ 分派表 ↔ handler 声明：两个方向都不许漏

## 判据

- `core_selftest` 的 `check_input_hold` 全绿：`0xfc` 清两张表 + 模数归 0 且不动掩码/键总数/扫描上限/游标/ip；`0x102` 上界 31/9 + 越界一槽不写 + 64 位键；`0x103` 只清当前帧槽（播种别的槽验证）+ 模数 0/负数原样；`0x104` 反向取最高位、上下文闸、掩码夹上限且未登记也不回滚、每条出口推进游标、模数 0 响亮失败；`0x105` 全 1 照写且不碰输入字。
- `tools/test/emulator-cxx-dispatch.test.mjs`。

## 落点

- `Frame::hold_slots`（键号 × 10 + 槽号 → 位置，−1 哨兵；64 位键）与引擎级三格 + `hold_cursor_by_slot_`（按**帧槽号**索引、`nullopt` = 引擎不建立初值）。
- handler 在 `vm_ops_io.cpp`；`0x104` 的三条出口共用 `advance_hold_cursor()`（先 32 位回绕再取余，`INT_MIN % -1` 响亮失败）。

## 非显然口径

- 游标**不在** `Frame` 上、按帧槽号存：帧释放/重载不复位。
- 上下文闸只比**编号**、不比帧号。
- `0x102` 的表与 `0xfb` 的按键回调表**互不可见**。
- 错误码是本仓自定（引擎文本只有一句、只提"第 1 个操作数"，不能当判据）。

## 未做

- 跨帧可见性用例（`0xfc` 清所有帧 / 跨帧不串表）需要切帧夹具 ⇒ 目前只在代码层面保证。
