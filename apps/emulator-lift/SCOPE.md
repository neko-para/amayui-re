# emulator-lift · SCOPE（显式偏差）

> 本文件只记**用户拍板的偏差**与"必须保留"的边界；不写引擎语义（那在 `data/spec/`），
> 不写进度（那在 `pnpm tools requirements plan`）。
> **不允许在用户没有明确要求的情况下编辑这个文件。**
> 依据：用户 2026-11-03 口径 + `docs/03-emulator/platform-effort.md` §4.1。

## 0. 判序

**本节 > 本文件其余各节 > `data/spec/`**。与 spec 冲突时按本节取舍；
被跳过的 spec 条款在 §2 点名，⛔ 不写进 spec 正文冒充引擎语义。

## 1. 可接受偏差（用户 2026-11-03）

| 偏差 | 做法 | 用户可见代价 | 仍必须保留 |
|---|---|---|---|
| **菜单** | 整体 no-op：`LoadMenuA` 返回非空假句柄、`SetMenu` 返回 TRUE、`TrackPopupMenu` 立即返回 0 | 无 Win32 菜单栏（游戏标题菜单不是它） | 窗口创建 / 消息泵 / 输入 / region / 计时器 |
| **模态** | no-op：`MessageBoxA` 返回 `IDOK`；AGERC 设置对话框整块不做 | 无"设置"对话框；关闭确认 / 存档命名直接继续 | 配置读写（INI/注册表）、存档本身 |
| **音频** | silent：播放 / 停止 / 音量接受调用并返回成功 | 无 BGM / 语音 / SE | `timeGetTime` / `timeSetEvent`、音频状态机的返回码 |
| **视频** | skip：影片 opcode 必须**推进**，不许挂起 | 无影片（LOGO/OP/剧情影片） | 影片 opcode 的状态推进与结束通知 |
| **Live2D** | 关闭（游戏内开关）作为第一可交付状态 | 模型不显示（固定立绘） | 如果开关只是脚本级跳过，SDK 仍链接但不被调用 |
| **3D** | 不做（若主线允许）：D3D9/D3DX 走 stub | 无 3D 战斗/场景 | 2D/ADV 主路径（DDraw + GDI） |

★ **3D 的前提**：若 Live2D 走 D3D 后端（`doc §6-2`），D3D9 不能砍；"3D 不做"与"保留 Live2D"互斥。

## 2. 被跳过的 spec 条款（点名）

- 菜单 / 模态：Win32 菜单与对话框族（`WM_COMMAND` 的菜单命令、`DialogBoxParamA` 等）。
- 音频：MCI CD-DA / MIDI / DirectSound 的设备语义；保留的是"请求 + 返回码"。
- 视频：MF 的 session / topology / EVR / DXVA2 内部；只保留影片 opcode 的状态推进。
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
