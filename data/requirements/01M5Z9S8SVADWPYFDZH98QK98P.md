# AGERC 插件桥 0x14b/0x14c/0x14d：核心 seam 还是 32 位宿主（需要决定）

- id: REQ-01M5Z9S8SVADWPYFDZH98QK98P
- type: spike
- status: open
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202

## 结论（本机可落地，缺一个决定）

`0x14b` / `0x14c` / `0x14d` **不是存档 IO**，是 **AGERC 插件桥**：

| opcode | handler | argc | 语义 |
|---|---|---|---|
| `0x14b` | `sub_4229D0` | 1 | op1 = **install 统一文件 id** → 路径 → `LoadLibraryA`（先 FreeLibrary 旧句柄），句柄存引擎一格 |
| `0x14c` | `422AB0` | 2 | op2 按**文本**读 → `GetProcAddress`；**解析成功后**才读 op1；0..99 之外抛 |
| `0x14d` | `sub_430170` | 6 | op1 → 函数格（**无越界检查**）→ 5 实参调用；op3 数组 `DEC → call → ENC`；op2 ← 返回值 |

三条各只出现 1 次，全在 `SAVE.BIN`（install id 51）开头的 dword 5/8/13，调 `_SetNameLenMax@20`。
`0x1a0` **不是**它们的前置（`0x1a0` 是"只读 292 字节头"的读侧指令，argc 9，三态 0/1/2）——
本节点原记录里"撞 0x1a0 seam"的时序是反的。

## 缺的决定

`AGERC.DLL` 是 **PE i386**，而 `emulator-cxx` 预设是 x64 ⇒ 真 `LoadLibrary` 装不进 x64 进程。三选一：

- **A（推荐）**：核心定一个 `ModuleHost`（load / resolve / call）注入 seam，平台实现**明确拒绝**并给诊断
  ⇒ 三条当场可收口，行为也与"x64 宿主上模块装不进来"的事实一致；
- **B**：按名字建模唯一导出（`_SetNameLenMax@20` 的体 = `*(u32*)DLLglobal = a2[0]; return 0;`）⇒ 能跑但**不是**引擎语义；
- **C**：32 位 shim / 子进程 ⇒ 只有这条要真机或 32 位工具链。

## 现状

`apps/emulator-cxx` 里三条**无 case、无 handler、无 TODO** ⇒ 走到 `SAVE.BIN` dword 5 就抛
`opcode-not-implemented:0x14b`。当前没有流程会走到那里（除非真存档 / 真读档路径被启用）。
