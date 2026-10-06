# 批 M7 · 真机探针（.NET 10，按新结构重写）

- id: REQ-01M3TCZ5A0D5ZYZQS47DZW2S3X
- type: req
- status: open
- parent: REQ-01M3TC9660ZG1YMB7KAKE4W94B
- order: 90

## 范围
`apps/inspector`：只读真机进程（OpenProcess + ReadProcessMemory），按新结构重写。

★ **别与模拟器混为一谈**（本轮澄清）：本题是**真机探针** —— 识别 AGE 引擎进程、定位 VM 解释器 `this`、
读全局表 / 帧快照（旧仓 C# / .NET 10 + WPF，`AmayuiInspector.{Core,Cli,App}`）。
**模拟器的窗口壳不是本题**：`app/amayui-emulator/electron/`（8 文件）是 `apps/emulator` 自己的表现层，
归 **M4-2**。两者只在"都按新结构重写"这条口径上相同。

## 子节点分工
| | 含什么 | 前置 |
|---|---|---|
| **M7-1 核心读取 + CLI 冒烟** `REQ-01M47T0SZQ28M1AA17M397J19N` | `AmayuiInspector.Core`（11 文件）+ `AmayuiInspector.Cli`（2 文件） | 无（不需要真机就能自检 DEC / 指纹 / 偏移） |
| **M7-2 WPF 壳** `REQ-01M47T0TNAG94EKC6FP2GPGQQR` | `AmayuiInspector.App`（22 文件：ViewModels / Views / Services） | M7-1（只读快照的对象形状由它定） |
| **M7-3 真机探针与观测** `REQ-01M47T0VC4QAQEN0K681TGA3NE` | `AmayuiInspector.Probe`（5 文件 / 108 KB）+ 真机取值 | M7-1；★ 还要**有真游戏在跑** |

## 判据
（待写：能在真机上取到指定全局槽并在报告里给出可再校验的观察。）
