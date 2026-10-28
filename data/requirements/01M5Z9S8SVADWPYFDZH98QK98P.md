# AGERC 插件桥 0x14b/0x14c/0x14d：核心 seam 还是 32 位宿主（需要决定）

- id: REQ-01M5Z9S8SVADWPYFDZH98QK98P
- type: spike
- status: dropped
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- dropped_reason: 用户口径（2026-10-28）：AGERC 明确不实现，视为外部系统（插件桥 = 平台边界，不是引擎语义）

## 决定（2026-10-28，用户口径）

**AGERC 不实现，视为外部系统。**

`0x14b` / `0x14c` / `0x14d` 是**插件桥**（`LoadLibraryA` / `GetProcAddress` / 按下标调导出），
不是存档 IO，也不是本仓要重实现的引擎语义。三条的接收者是一个**外部模块**（`AGERC.DLL`，PE i386）；
`AGERC.DLL` 的**内容**不在本仓范围内（它自己的重建另有单，且已 `dropped`）。

## 落点（规范侧怎么摆）

- 这三条在 spec 里属于**平台边界**：只写"把工作交给外部模块的哪个导出"，**不写**那个模块内部做什么。
  与「平台 API 是 spec 的边界」同一口径（渲染 / 窗口 / 声音请求都是这样摆的）。
- 模拟器侧：**不实现**，落到"未实现指令"是**可接受**的终态（不是欠账）；若将来某条脚本真的走到它，
  应当**响亮失败**（`opcode-not-implemented`），而不是静默跳过。
- ⛔ 不要为它们造 32 位 shim / 子进程：那是在重实现一个明确排除的系统。

## 已完成

- 需求树：本条按本决定 `dropped`。
- 取证结论留在 `.tmp/forensics/save-0x14b-0x14d.md`（handler / argc / 读序 / 失败分支 / 唯一调用点），
  将来若要重开，从那里起。
