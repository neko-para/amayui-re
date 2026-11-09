# 修 EU-ROOM 退出的 C++ EH 展开：RaiseException / RtlUnwind 未实现 ⇒ Command_Exit_Exception 一抛就停

- id: REQ-01M708DF24KY0QZNEHY66S0HYW
- type: bug
- status: open
- parent: REQ-01M6XVYY5TCKSAS80WS7PADF8P
- repro: apps/emulator-lift/src/shims/kernel32_bootstrap.cpp#RaiseException
- severity: S2
- tags: [emulator-lift, seh, exception]

## 现象

`EU-ROOM` 点「戻る」/ 按 ESC 走退出路径：后缓冲 `LockRect`/`UnlockRect` 已经通过（`REQ-01M6ZDSGQY1RXX3N3WJKA8D26H`，已收口），
但 `room-exit.graph` 仍 **exit 5**、没有 `task done`，停在新 gap：

```
stop      : stopped @ eip=004123C4
detail    : unimplemented kernel32!RaiseException
返回地址   : 0x004123C9（sub_412290 / WM_TIMER 2 路径）
```

## 根因（已完成的部分）

`0x4123C4` 是 `__CxxThrowException(Command_Exit_Exception)` 的调用点（抛异常的 `sub_428A60` 就是 exit/reset 例程，
见 `REQ-01M6XVZ8AXJNG492WA2QAMTA35` 的只读分析：它读 `set:EnableMemFlip/DrawMode/CreateObject/AutoFreeTex` 四个键 + 拆池 + `sub_40ED40` 重装）。
本线没有实现 SEH / C++ EH 展开（`RaiseException` / `RtlUnwind` / `_except_handler` 一族）⇒ 一抛就停。

## 判据（修好后）

- `room-exit.graph` 真机跑：**exit 0** + trace 末尾 `task done`，终态 = TITLE 稳定帧 hash（与本单无关的 戻る 点击另见 `REQ-01M6ZDSPVYS3R78TK8TFKVBC8N`；
  用 ESC 走同一条路径即可验收）。
- 至少一条 pure 守卫：夹具里造一次 `RaiseException` + `RtlUnwind`（或最小 C++ throw）⇒ 展开到 catch / 或按"本线不支持"给出**响亮且可区分**的停因，
  ⛔ 不许把 `RaiseException` 变成静默 no-op（那会让 guest 的 catch 块永远不执行）。
- 变异：把展开判据短路 ⇒ 该用例红。

## 边界

- ⛔ 不改 guest 可见的异常记录 / 栈布局（要按 x86 SEH 文档实现，先取证）。
- ⛔ 不用"跳过这条退出路径"换绿（那只是不测）。
- 这条是 A 单终态判据的前置：`REQ-01M6XVYY5TCKSAS80WS7PADF8P` 的"出房间"挂在它 + 戻る 那张单上。

## 与别的单的关系

- 同一条退出路径：`REQ-01M6ZDSGQY1RXX3N3WJKA8D26H`（后缓冲 LockRect，已收口）· `REQ-01M6ZDSPVYS3R78TK8TFKVBC8N`（戻る 点击，open）· **本单**。
- `sub_428A60` 的职责与分段见 C 单（`REQ-01M6XVZ8AXJNG492WA2QAMTA35`）的只读分析。
