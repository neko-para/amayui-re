# emulator-cxx · 第二批：帧/本地块/标志/数值/场景读回 + 暂停门 + 0x23C（24 条）

- id: REQ-01M5T35HVTX5RW91RKDV3DABGB
- type: req
- status: done
- parent: REQ-01M5SZ3M7GCR0M23RSYBD7GEA5
- verify: tools/test/emulator-cxx-dispatch.test.mjs#★ 分派表 ↔ handler 声明：两个方向都不许漏

## 判据

- `ctest` 5/5；`core_selftest` 的 `check_second_batch` / `check_scene3d_setters` / `check_pause_and_clock` / `check_run_bracket` 全绿。
- `tools/test/emulator-cxx-dispatch.test.mjs`（分派表 ↔ handler 声明两个方向 + 17 条 TITLE 路径钉）。

## 本批落了什么（26 条）

- 帧 / 本地块：`0x008` call-frame、`0x07B` 两目标、`0x07C` 离开、`0x199` 进入（深度判据 = **本帧身份编号**，存档字第 25/26 位）。
- 标志 / 状态 / 模式 / 层：`0x19A`、`0x1B6`、`0x1B7`（三个互不相同的格）· `0x1C7`（位 27 → 0/1）· `0x1CC`（状态格，原值）· `0x1AD`（落点格复位值 −1）。
- 数值 / 随机 / 参数 / 时钟 / 取址 / 池拷贝：`0x060`（计数器 → 随机 → 范围 → 落点；范围 0 先写 0 再抛）· `0x063` lea · `0x074`/`0x07F`（参数格，未写过 ⇒ 响亮失败）· `0x0D0` · `0x191`/`0x2D0`/`0x2D1`/`0x2D3` · `0x1B0`。
- 场景读回：`0x218`/`0x21A`（两个三元向量，只用 `scene_records_`）· `0x228`（平移分量，标志最后写）· `0x21D`（三张表各自复制，成功才标脏）。
- **3D setter**：`0x21E`（三个浮点 ÷100 进缩放矩阵）· `0x21F`（轴角另存 + 度 → 弧度进旋转矩阵）；记录「已存在」位为 0 时**只建记录、一个字段都不写**。★ 旋转矩阵的元素布局 / 轴归一化方式规范**未成篇** ⇒ 实现选择（行主序 + Rodrigues）已在注释标明。
- 主循环：**暂停 = 区间忙标记**（同一格；忙且等待位 = 0 时场景推进 / 影片计时 / 门前渲染整段跳过）· `0x23C` 无条件重盖时钟对 · 门一直关且挡门的位**没有服务分支** ⇒ `StopReason::GateStalled` + 一次性诊断（`gate-bit-unserviced:0x…`）。

## 非显然口径

- `0x1C7` 的 "=1" 与 `0x199` 的"已在块内"形态在 `step()` 路径上**不可达**（第 26/27 位没有服务分支）；守卫写的是"置起即 GateStalled"。
- `0x7C` 成功退出后门字带第 26 位 ⇒ 必然 GateStalled：这是规范表现，不是 bug。
- `0x21D` 只置脏、不置「有动画」；没有源时上报且一张表都不改。
- 自检二进制的栈预算：把十几台 `Machine` 放进同一栈帧（Debug /Od 不复用栈槽）会撞 1 MiB，`CMakeLists` 对**测试**显式给 `/STACK:8388608`（不是产品设置）。
