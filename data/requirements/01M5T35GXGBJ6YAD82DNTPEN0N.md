# emulator-cxx · 键盘 + 鼠标接入（不接摇杆）

- id: REQ-01M5T35GXGBJ6YAD82DNTPEN0N
- type: req
- status: done
- parent: REQ-01M5SZ3M7GCR0M23RSYBD7GEA5
- done_reason: 键盘 + 鼠标接入完成（不接摇杆）：宿主键位表 kKeyBits + SDL 键盘/鼠标 → 输入字 / 客户区光标 / 按钮位图 / 事件队列；0x109 / 0x108 / 0xcc / 0xcd 由 selftest 的 check_input_devices 守卫；门禁 ctest 5/5、pnpm test 1071/0。规范侧的物理键映射属平台边界（不进 spec）。

## 判据

- `ctest --test-dir apps/emulator-cxx/out/build/windows-x64` 5/5。
- `emulator-core-selftest` 的 `check_input_devices`：`0x109` 光标（可用 / 不可用写 −100000）、`0x108` 按钮位图（不可用写 0）、`0xcc` 登记、`0xcd` 时间闸（`elapsed >= interval` 含等号）与目标 −1 撤销。
- 宿主路径：`window.cpp` 每外层迭代泵事件，SDL 键盘 / 鼠标归约成输入字 / 客户区光标 / 按钮位图 / 一条事件队列。

## 口径

- 只接**键盘 + 鼠标**（**不接摇杆**）。
- 物理键 ↔ 输入位号的映射是**宿主策略**（`window.cpp` 的 `kKeyBits` 一张表）：规范明说「物理键与输入位号的映射」属平台边界 ⇒ ⛔ 不进 spec。Esc 仍直接关窗（同时映射到"取消"位 1）。
- 客户区坐标按窗口尺寸缩放到逻辑分辨率（1280×720）；事件标志 / 附加量的位段规范不规定，宿主用 `0x11`/`0x12`（按钮）/`0x40`（滚轮），`extra` = 按钮序号或滚轮增量。

## 未做

- 摇杆（不在范围）· IMU / 触屏 · 输入的事件时间戳（规范不规定）。
