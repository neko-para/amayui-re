# 标准句柄三元组自相矛盾：SetStdHandle 不存值 / GetFileType 全报字符设备 / GetConsoleMode 全成功

- id: REQ-01M6K69K9HM3ETCDNMJWPHGXJC
- type: bug
- status: open
- parent: REQ-01M6H6FW5JGQGHACXHPWZD4Y3E
- repro: apps/emulator-lift/src/shims/kernel32_bootstrap.cpp#shimSetStdHandle
- severity: S3
- tags: [emulator-lift, m1, s6]

## 一句话
标准句柄三元组是**互相矛盾的空实现**：`SetStdHandle` 不保存任何状态、`GetStdHandle` 永远返回三个写死的假句柄、`GetFileType` 对**所有**句柄（含磁盘文件）报 `FILE_TYPE_CHAR`、`GetConsoleMode` 对**所有**句柄报输入模式 —— 三者叠加会让 CRT / guest 把文件句柄当控制台。

## 现状（2026-11-04 审计）
`apps/emulator-lift/src/shims/kernel32_bootstrap.cpp`：
```cpp
void shimSetStdHandle(...) { call.result = 1; }                 // 不存值
void shimGetFileType(...) { call.result = 2; }                  // 永远 FILE_TYPE_CHAR
// std 句柄：stdin=0x0C / stdout=0x10 / stderr=0x14（固定）
// GetConsoleMode：对所有句柄返回输入模式 + 成功
```
- 真实的 `SetStdHandle` 改的是进程级表，之后 `GetStdHandle` 要返回它；`GetFileType` 应按句柄类型返回 `FILE_TYPE_DISK(1)` / `FILE_TYPE_CHAR(2)` / `FILE_TYPE_PIPE(3)`；`GetConsoleMode` 对非控制台句柄必须 `0 + ERROR_INVALID_HANDLE(6)`。
- 当前 `CreateFileA` 拿到的文件句柄如果被 `GetFileType` 查询，会得到"字符设备"；CRT 的 `_ioinit` / `isatty` 类判断会走错分支。
- 启动到 L2 仍绿（A4 未触发反例），但它属于 D6"文件与路径 / 进程·模块"交界，且登记为 implemented。

## 判据（落地时写成 CTest）
- `CreateFileA` 打开一个真文件 ⇒ `GetFileType` 返回 1；`GetConsoleMode` ⇒ `0 + 6`；
- `SetStdHandle(STD_OUTPUT, h)` 后 `GetStdHandle(STD_OUTPUT)` 返回 `h`；恢复原值也要成立；
- 未设置的 std 句柄仍按"字符设备"返回（保持启动路径现状）；
- 加一条变异（把 `GetFileType` 改回恒定 2 ⇒ 新用例红）。

## 边界
- 管道 / 控制台真设备不在本单；只要求"文件句柄 ≠ 字符设备"和 `SetStdHandle` 真的存值。
