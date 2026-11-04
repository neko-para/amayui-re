# emulator-lift · SCOPE（显式偏差）

> 本文件只记**用户拍板的偏差**与"必须保留"的边界；不写引擎语义（那在 `data/spec/`），
> 不写进度（那在 `pnpm tools requirements plan`）。
> **不允许在用户没有明确要求的情况下编辑这个文件。**
> 依据：用户 2026-11-03 口径 + `docs/03-emulator/platform-effort.md` §4.1。

## 0. 判序

**本节 > 本文件其余各节 > `data/spec/`**。与 spec 冲突时按本节取舍；
被跳过的 spec 条款在 §2 点名，⛔ 不写进 spec 正文冒充引擎语义。

## 1. 可接受偏差（用户 2026-11-03；临时文件名序号 2026-11-04）

| 偏差 | 做法 | 用户可见代价 | 仍必须保留 |
|---|---|---|---|
| **菜单** | 整体 no-op：`LoadMenuA` 返回非空假句柄、`SetMenu` 返回 TRUE、`TrackPopupMenu` 立即返回 0 | 无 Win32 菜单栏（游戏标题菜单不是它） | 窗口创建 / 消息泵 / 输入 / region / 计时器 |
| **模态** | no-op：`MessageBoxA` 返回 `IDOK`；AGERC 设置对话框整块不做 | 无"设置"对话框；关闭确认 / 存档命名直接继续 | 配置读写（INI/注册表）、存档本身 |
| **音频** | silent：播放 / 停止 / 音量接受调用并返回成功 | 无 BGM / 语音 / SE | `timeGetTime` / `timeSetEvent`、音频状态机的返回码 |
| **视频** | skip：影片 opcode 必须**推进**，不许挂起 | 无影片（LOGO/OP/剧情影片） | 影片 opcode 的状态推进与结束通知 |
| **线程 / 定时器队列** | **guest 单线程**：`CreateThread` / `ExitThread` / `ResumeThread` / `SetThreadPriority` / `SetThreadAffinityMask` / `GetProcessAffinityMask` / `FreeLibraryAndExitThread` / `CreateTimerQueue` / `CreateTimerQueueTimer` / `DeleteTimerQueueTimer` 一律**响亮失败**（`addUnimplemented`，执行到就 trap） | 无（业务线程不存在；服务线程被音频 silent / 视频 skip 消化） | 主线程的同步原语 / 等待 / 临界区、单实例信号量、`timeSetEvent` 的计时语义 |
| **SEH / C++ 异常** | **不展开**：`RaiseException` / `RtlUnwind` 一律**响亮失败**（`addUnimplemented`，执行到就 trap）；`_CxxThrowException` 走同一条"未处理异常"路 | 异常路径**不恢复**（进程终止并留下诊断） | 失败诊断（EA / 访问类型 / 区域）——异常发生在成功路径上会**当场暴露**，不静默 |
| **Live2D** | 关闭（游戏内开关）作为第一可交付状态 | 模型不显示（固定立绘） | 如果开关只是脚本级跳过，SDK 仍链接但不被调用 |
| **3D** | 不做（若主线允许）：D3D9/D3DX 走 stub | 无 3D 战斗/场景 | 2D/ADV 主路径（DDraw + GDI） |
| **临时文件名序号** | `GetTempFileNameA(uUnique=0)` 从 **0** 起递增找空号（真机从系统时间起）；`uUnique!=0` 按文档用调用方的值、**不创建**文件 | 具体序号与真机不同（可复现优先于逐值一致） | 名字形态 `<pre><uuuu>.TMP`（前缀只取 3 字符）、`uUnique!=0` 返回原值且不创建 |

★ **3D 的前提**：若 Live2D 走 D3D 后端（`doc §6-2`），D3D9 不能砍；"3D 不做"与"保留 Live2D"互斥。

★ **线程整族不做的依据与退路**（2026-11-04 用户批准记入本文件）：`README.md` §2.5 已定 guest 单线程；
`.lst` 里的线程全是**服务线程**（DirectShow 读文件 / 声音 / MF 回调 / 多媒体定时器），而本文件已把
音频判 silent、视频判 skip ⇒ 它们**不该被创建**。判据与退路见 `docs/plan-m1-cpu-base.md` §4 D5 / §9 R4：
**若成功路径真的创建线程 ⇒ 记录并开单**，⛔ 不在本线直接开并发（那要先有证据、再改 §2.5 的单线程口径）。
白名单落点：`tools/lift.mjs` 的 `FAMILY_STUB_APPROVED` **只列 PE 导入表里真出现的键**
（`CreateRemoteThread` / `_beginthreadex` 不在导入表 ⇒ 不进白名单，否则 `lift check` 会报"白名单漂了"）。
★ **档位**：现行实现是 `addUnimplemented`（**硬 trap**：打印 `module!name` + 最近指令后停下）；plan §4 D5 的括注写的是
"打日志、返回失败或空句柄"（软 stub）—— 两者对 guest 的后果不同（继续跑 / 停下）。本行登记的**是硬 trap**；
若改成软 stub 属新裁决，不在本次登记范围内。

★ **异常也按"发生即失败"登记**（2026-11-04 用户）：`RaiseException` / `RtlUnwind` 不做 SEH 展开，执行到就 trap
（plan §9 R1：M1 只做"未处理异常 = 响亮失败 + 完整诊断"）。**若成功路径也用异常做流程控制** ⇒ 单独开单再议，
⛔ 不先假装恢复（那会把"异常没实现"变成静默的数据路径偏差）。

## 2. 被跳过的 spec 条款（点名）

- 菜单 / 模态：Win32 菜单与对话框族（`WM_COMMAND` 的菜单命令、`DialogBoxParamA` 等）。
- 音频：MCI CD-DA / MIDI / DirectSound 的设备语义；保留的是"请求 + 返回码"。
- 视频：MF 的 session / topology / EVR / DXVA2 内部；只保留影片 opcode 的状态推进。
- 线程 / 定时器队列：线程创建 / 线程调度 / 亲和性与优先级 / 定时器队列的线程语义；
  保留主线程的同步原语与等待、`timeSetEvent` 的计时。
- 异常：SEH 展开 / `.xdata` handler / C++ 异常展开的语义；保留"未处理异常 = 响亮失败 + 完整诊断"。
- Live2D：模型 / 动作 / 渲染接口（SDK 内部）；若走游戏 renderer，则不在平台层。
- 3D：D3D9 设备状态机 / D3DX effect / `.x` 层级；只保留 2D/ADV 路径。

## 3. stub 的硬要求

每处 stub 必须：

1. 返回**合理值**（`HMENU` 非空、`SetMenu` TRUE、`MessageBoxA` IDOK、`HRESULT` 成功）；
2. **不阻塞、不等待**（`TrackPopupMenu` 立即返回；影片 opcode 推进）；
3. 打一行日志（可回查）；
4. 在覆盖清单 `data/lift/overrides.json` 里登记 `kind: stub` + `reason`。

⛔ 不许静默 no-op；⛔ 不许用 stub 掩盖"该实现的路径"。

## 4. 实现策略（不是偏差）

- 执行源 = `.lst`（guest-state）；`.c` 只作阅读对照（`doc §5.6`）。
- 执行单位 = basic block；`jmp/call/ret` 只改 `eip` 回 dispatcher（`doc §5.7`）。
- 宿主 = SDL3；Win32/GDI/DDraw 形状层自己写（`doc §5.5`）。
- 覆盖 = 清单 + 块级调度表；生成物不入库（`doc §5.7`）。
