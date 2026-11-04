# 标准句柄三元组自相矛盾：SetStdHandle 不存值 / GetFileType 全报字符设备 / GetConsoleMode 全成功

- id: REQ-01M6K69K9HM3ETCDNMJWPHGXJC
- type: bug
- status: done
- parent: REQ-01M6H6FW5JGQGHACXHPWZD4Y3E
- verify: apps/emulator-lift/tests/file_io.cpp#std-handles-triple
- repro: apps/emulator-lift/src/shims/kernel32_bootstrap.cpp#shimSetStdHandle
- severity: S3
- tags: [emulator-lift, m1, s6]

## 一句话
标准句柄三元组此前是**互相矛盾的空实现**：`SetStdHandle` 不存值 / `GetFileType` 对所有句柄报
`FILE_TYPE_CHAR` / `GetConsoleMode` 对所有句柄成功 ⇒ CRT 把磁盘文件当控制台。**已修**。

## 改了什么
- `src/runtime/handles.h`：句柄表上加了**标准句柄槽**（`kStdSlots = 3`，默认 `0x0C / 0x10 / 0x14`）+
  `stdHandle` / `setStdHandle` / `stdSlotOf` / `resetStdHandles`。
  为什么放这里：**一台机器一份**（`ShimState` 在 `x86.h`，本轮的文件清单不动它）⇒ 同一个测试进程里
  两个 Fixture 不会互相污染。
- `src/shims/kernel32_bootstrap.cpp`：
  · `GetStdHandle` 读槽；非法 `nStdHandle` ⇒ `0` + 6。
  · `SetStdHandle` **真的存值**（非法 `nStdHandle` ⇒ `0` + 87）。
  · `GetFileType`：先看句柄表的真实种类（`File` ⇒ `FILE_TYPE_DISK(1)`），再看标准槽（⇒ `FILE_TYPE_CHAR(2)`），
    其余 ⇒ `FILE_TYPE_UNKNOWN(0)` + `ERROR_INVALID_HANDLE(6)`；非文件的**内核对象**句柄 ⇒ 0 + 一行日志。
  · `GetConsoleMode`：只对标准槽里的假句柄成功（启动路径现状）；真句柄 ⇒ `0` + `ERROR_INVALID_HANDLE(6)`；
    `lpMode == 0` ⇒ `0` + 87。

## 判据（已落成 CTest）
- `apps/emulator-lift/tests/file_io.cpp#std-handles-triple`
  `CreateFileA` 的真文件 ⇒ `GetFileType=1`、`GetConsoleMode=0`+6；`SetStdHandle(STD_OUTPUT, h)` 后
  `GetStdHandle(STD_OUTPUT)` 返回 h、恢复原值也成立、别的槽不受影响；没动过的标准槽仍 `GetFileType=2`
  且 `GetConsoleMode` 成功；非法 `nStdHandle` ⇒ 0 + 6。

## 证据
- 变异实测：`GetFileType` 的 `File` 分支改回 `kFileTypeChar` ⇒ `std-handles-triple` 红。

## 边界 / `<待核>`
- 管道 / 真控制台设备不在本单（`FILE_TYPE_PIPE(3)` 目前没有产生者）。
- 非文件的**内核对象**句柄（事件 / 互斥量 / 查找 / GDI 对象）⇒ `FILE_TYPE_UNKNOWN(0)` + 一行日志：
  与 Wine / 真机"不是文件对象"的口径一致，但**未在真机采样**。
- `SetStdHandle` 非法 `nStdHandle` = 87、`GetConsoleMode(lpMode=0)` = 87 也都是 `<待核>`（MSDN 没点名）。
