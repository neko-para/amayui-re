# 🐞GAME START 后 halt：0x192 落点 type 14 未支持（write-text-unsupported-tag:14）+ halt 帧链诊断

- id: REQ-01M645H4KVNJMSK6Z3AX7FJYZB
- type: bug
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: apps/emulator-cxx/tests/core_selftest.cpp#0x192 落点 type 14 没有写穿指针目标
- repro: apps/emulator-cxx/tests/core_selftest.cpp#0x192 落点 type 14 没有写穿指针目标
- severity: S1

## 现象（用户口径 2026-10-29）

Title → Game Start → 开始游戏 之后脚本停机，日志只有一句：

```
[render] script halted reason=8 opcode=0x192 message=write-text-unsupported-tag:14
[render] play frames=650 clock=11571ms script=94 windows=0 maxWindows=18
[core] exit user_quit=0 halted=1 reason=8 opcode=0x192 message=write-text-unsupported-tag:14
```

用户同时指出：**日志里只有 opcode id，没有"哪个 BIN 的哪个偏移"**，拿到这句无法查询、也无法反馈。

## 根因

- `0x192`（set-string）在实现里走共享原语 `write_text`，而它**只认落点类型 5 / 11**，其余一律抛
  `write-text-unsupported-tag:N`。
- 引擎的落点分派接受 **5 / 8 / 11 / 14**（EA `0x433310`），四条都汇到**同一个记录级写入**
  （EA `0x43362F` = `sub_40C120(记录, -1, 0, 源)`）⇒ 类型 8（全局）/ 14（局部）只是"按指针取记录"：
  槽里装的是**字符串记录的地址**，解引用后写它指向的那一条。
- 触发点：`script=94` = ALF TOC 第 94 项 = `UNITECH.BIN`；它**唯一**一条 `0x192` 在**文件偏移 3236**
  （body 3176），`dest{type=14,raw=0}` / `src{type=14,raw=1}`；调用者是 `GAMESTART.BIN`（21099）里的
  `call-script 94`。
- 全库机械枚举（330 个脚本 / 13677 条 `0x192`）：类型 5 = 11106、11 = 124、**14 = 2447**、容器句柄 = 0
  ⇒ 不是边角料（TITLE.BIN 的菜单标签就是 type 14）。

## 修法

1. `write_text` 增加 **8 / 14**：读指针槽 → 解析成（池, 下标）→ 写那条字符串记录。指针槽没写过 /
   指向非字符串池 ⇒ **响亮失败**（引擎不解引用判空、不查池界，那是 UB；⛔ 不许静默写 0 号记录）。
2. halt 诊断补**帧链**：新增 `Machine::stop_stack_report()`，逐层打印
   `[stack] #<深> frame=<号>/40 script=<id> bin=<BIN 名> ip=<body 内> file=<文件内偏移> opcode=<该位置指令> [call_at=<dword>]`。
   四个打印点（窗口 `[render]`、boot 的两处 `[core] exit`、headless 的 `[play]`）在**异常停机**时各刷这一段。
   - `Frame` 新增 `body_offset`（脚本头 body 起址，`load_root` / `0x03` / `0x06` 写）与 `call_sites`
     （与 `ret_stack` 同深的"在哪条指令上登记的返回点"）。
   - BIN 名取自 ALF TOC（`install.index.files[script_id].name`，CP932→UTF-8），不需要新表 / 新参数。
   - ⛔ 不动现有 `[render]/[core]/[play]` 的格式，只加行。

## 验证

- 守卫（CTest `emulator-core-text-pointer-write`，`apps/emulator-cxx/tests/core_selftest.cpp`）：
  ① 落点 type 14 经 `0x61` 指向 global-string 0 ⇒ 写穿（读回 = 源）；
  ② 源侧 type 14 能读出目标记录；
  ③ 空指针槽 ⇒ 响亮失败且不写 0 号记录；
  ④ 落点 type 3 仍抛（放宽只针对 8 / 14）；
  ⑤ `stop_stack_report()` 必须含 `frame=` / `script=` / `ip=` / `file=` / `opcode=0x192`。
- **变异实测**：把 `write_text` 的 8/14 分支关掉 ⇒ 该 CTest 当场红，报错文本正是用户拿到的那句
  `write-text-unsupported-tag:14`。

## 归因

**spec 缺失**（不是实现偏离）：旧正文写"落点 8 / 14 本版不规定"，实现于是响亮失败。本轮把
`CORE-OPS-TEXT-SET`（`0x192`）与 `0x2eb` 的落点写进合同（证据：EA `0x433310` 分派 + `0x43362F` 公共收尾 +
`0x4348BE` 显示 `0x2eb` 调同一个写原语），并把 `40-pools` 的"type 8 / 14 只有取址"限定为**数值原语**。

## 仍欠

- `0x8005` / `0x800B`（字符串容器句柄变体）的写入路径仍未取证；全语料 `0x192` 没有这种站点
  （残差 `RES-STRING-SET-LOCAL`）。
- 指针槽指向**非字符串池**时的引擎行为是 UB，本版不规定；实现选择响亮失败。
- 帧链里的"最近 call"记的是**返回点登记**（`0x8d` 的 call 与几条回调登记共用一条栈），不是完整的
  调用图。
