# 时钟零点：GetLocalTime/GetSystemTime 把宿主 uptime 当 1970 epoch（存档时间 1970/1/6 21:23）

- id: REQ-01M7NC46ZFG61JZ0X78AQ9BRSW
- type: bug
- status: done
- parent: REQ-01M7JHSE82SRRH0QJ7G6GXRD0Y
- verify: apps/emulator-lift/tests/kernel32_time.cpp#wall-vs-uptime-split
- repro: docs/03-emulator/defects.md#BUG-30
- severity: S3
- tags: [emulator-lift, shim, kernel32, time]

## 一句话

`GetLocalTime` / `GetSystemTime` 现在把宿主 **uptime** 当 1970 epoch 展开（存档显示 `1970/1/6 21:23`，随开机时长漂移）。
改成**墙钟**（`system_clock` 起步）+ `GetLocalTime` 做宿主时区；⛔ 不动 `GetTickCount` / `timeGetTime` / QPC 的 uptime 语义。

## 现象 / 根因

- 现象：存档列表时间 = `1970/1/6 21:23`（SAVE03.DAT header `+0x108`，16 B `SYSTEMTIME`）；同机不同开机时长得到不同 1970 日期。
- 根因：`runtime/clock.h` 的 `rawNow100ns()` = `steady_clock`（零点=开机），`now100ns()` 无 epoch；
  `shims/kernel32_time.cpp` 直接按"1970-01-01 起"展开。AOT 臂强制 real-time。
- 写侧 `0x4377B6 GetLocalTime` / 读侧 `0x438120`、`0x42D700` 都没有错；次因是 `GetLocalTime == GetSystemTime`（无时区）。
- 完整读数与 EA：`docs/03-emulator/defects.md#BUG-30`。

## 要做

1. `clock.h`：新增**墙钟**读数（real 档 = `system_clock` 起步的 epoch + 单调增量；虚拟档 = 固定 base + 虚拟 now），
   ⛔ 不改 `now100ns()` / `nowMs()`（`GetTickCount` / `timeGetTime` / QPC 仍用 uptime）。
2. `kernel32_time.cpp`：`GetSystemTime` = UTC；`GetLocalTime` = UTC → 宿主时区；`GetSystemTimeAsFileTime` = 1601 delta；
   `GetThreadTimes` 的创建时间用墙钟。
3. 测试：注入墙钟 base + 时区；断言 `GetLocalTime - GetSystemTime == 时区偏移`、`SYSTEMTIME` 字段与注入值一致、
   `FILETIME` 往返一致；⛔ 不断言"当前年份"。
4. 变异：把墙钟读数换回 uptime ⇒ 新用例红。
5. 存档侧验收：用 SAVE03 的 run 读 header，年份不再是 1970（读数进报告，⛔ 不是 test 里写死 2026）。

## 判据

- 新 `kernel32_time/*` CTest 通过；既有 `GetTickCount` 相关用例不变。
- 变异实测红。
- 存档 header 的 `wYear` 与宿主墙钟一致（同日即可，允许跨零点）。

## 未做

- 不改 AOT 强制 real-time 的裁决。
- `time` / `_time64` 在 AGE.EXE 里没有导入，本单不新增。
