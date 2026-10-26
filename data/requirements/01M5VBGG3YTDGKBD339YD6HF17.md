# 输入契约订正：0xcc/0xcd 身份 · 0x109 哨兵 · 0x108 右键位

- id: REQ-01M5VBGG3YTDGKBD339YD6HF17
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: apps/emulator-cxx/tests/core_selftest.cpp#check_input_devices
- tags: [input, spec]

## 范围

0x12e 之外的输入契约偏差（同一条 TITLE 鼠标回调路径）：

1. `0xcc` / `0xcd` 的登记身份：引擎存/比 `frame+0x3C` = frameArg（打开该脚本的统一文件 id），不是“调用层数”，也不是帧号。spec 与 emulator-cxx 都写错。
2. `0x109` 的 `-100000` 哨兵：引擎只在输入轮询被停用（`Input[1688] == 0`）时写它；窗口存在但光标在客户区外时返回真实坐标（可为负）。emulator-cxx 在 `inside == false` 时写哨兵。
3. `0x108` 右键位：引擎用 `SM_SWAPBUTTON` 把左右映射到 bit0/bit1；emulator-cxx 直接传 SDL 掩码，右键落在 bit2，TITLE 的 `and 3f2, 2` 永远看不到右键。

## 判据

- `0xcc` 在脚本 A 登记、切到脚本 B 后 `0xcd` 必须抛身份不符；回到 A 未重登记也不得放行。
- `0x109`：光标在客户区外仍写真实负坐标；只有输入源不可用才写 `-100000`。
- `0x108`：SDL 右键按下必须让引擎位图 bit1 置起（SDLK 交换设置属平台边界）。

## 备注

`0x2fc`（触点查询，不是鼠标事件队列）、键总数初值 7、`0x101` 采样、`0x202/0x203` 负值归一化另列后续单，不在本单最小闭环内。
