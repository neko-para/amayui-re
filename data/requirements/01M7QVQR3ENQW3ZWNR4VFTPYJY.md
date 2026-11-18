# 🐞S3 `PresentInterval` 读了不消费：guest 把配置改成 1/2 时真机被 vblank 挡住、我们立刻返回

- id: REQ-01M7QVQR3ENQW3ZWNR4VFTPYJY
- type: bug
- status: open
- parent: REQ-01M7JHSD5MWTB9N8WJC2Y2T69M
- repro: apps/emulator-lift/src/runtime/d3d9.cpp#presentation_interval
- severity: S3
- tags: [emulator-lift, dshow, d3d9, frame-pacing]

## 现象 / 提问（用户口径 2026-11-19）

模拟器在部分场景跑到 **100 fps**（mac 上 130），怀疑真机有垂直同步把它压在 60。

## 已核（反汇编锚 EA；全文 `.tmp/fps-cap/engine.md` + `.tmp/fps-cap/shim.md`）

- **引擎主动要 IMMEDIATE**：主渲染器 `sub_498CC0` 的 `PresentationInterval` 唯一写入点是配置
  `display:PresentInterval` 的查表：`0x498FBF → 80000000h`(0) / `1` / `2` …；影片渲染器 `sub_47C790`
  在 `0x47C7C3 mov dword ptr [esi+34h], 80000000h` —— 纯立即数、无分支。**写 interval 不经过任何能力探测**
  ⇒ 宿主 caps 只声明 IMMEDIATE 不改变引擎写的值。`FullScreen_RefreshRateInHz` 恒 0（DEFAULT）。
- **引擎按墙钟 + 内容帧时钟推进**：主循环 `sub_412290` 用 `timeGetTime`（`0x412408 mov ebx,timeGetTime`，
  按函数名 grep 会漏）、`PeekMessageA` 非阻塞、`Sleep(0)`；QPC 导入但零调用；无 60/16 常量、无 fps 计数。
  内容帧时钟 `sub_453AF0`：`还差 = 帧序号×每帧ms − elapsed`，≥5ms 不画、0<还差<5ms `Sleep`、`idiv` 算跳帧数；
  每帧毫秒来自**命令表**（`0x41D743 call sub_41BF50`）⇒ 这就是用户观测到的稳定 30 fps / 10 fps。
- **帧序号是算出来的**（`0x453B44 idiv`）⇒ 机器更快不会让剧情变快。
- **每帧呈现不走 DDraw**（`Flip` 不存在）⇒ `DDFLIP_NOVSYNC` / `WAIT` 与本题无关。
- 引擎自写的 `SYS4REG.INI` **72/72 份都是 `[display] PresentInterval=0`**。
- "跑满"的场合 = 脚本段既无动画位（0x08/0x10/0x80）又无空闲位（0x400000）且不阻塞（连续执行 / 静态画面）
  ⇒ 主循环空转、每轮 Present；等输入阻塞在 `0x41285C WaitMessage`，等演出被帧时钟挡住。

## 裁决（2026-11-19）

**不加快率上限、不按 60 限速。** 真机请求的就是 `IMMEDIATE`（不等 vblank），所以"能跑多快就跑多快"是
**与真机一致的语义**；加 60 会**背离**真机。既有口径（`REQ-01M6P6YSTMC3MCDTHS7RSQYRRE`「帧率无上限」）
因此有了更强的证据支撑。`--fps-cap` 若做，只能 opt-in 诊断，⛔ 不进默认档、⛔ 不进 A/B 判据（§12.6）。

**本单剩下的唯一问题**：`IDirect3DDevice9::Present` 的 `PresentationInterval` 我们**读了不消费**
（`d3d9.cpp` 读进结构体后全仓零使用，也不进设备状态块）。默认配置（`PresentInterval=0`）下这是**无差别**的；
只有当用户把配置改成 `1` / `2`（真机会被 vblank 挡住）时才会分叉。

## 方案（未做，待裁决）

尊重 guest 的 interval 需要"等 vblank"——我们的宿主没有 vsync 源，只能**近似**（按刷新率算 deadline 阻塞）。
落点（`shim.md` §6）：语义判定只能在 `runtime/d3d9.cpp` 的 `present()`，另需把 interval 落进设备状态块
（`createDeviceState`），共 ~3 文件 <20 行。⚠ "用 16.67ms 近似 vblank" 是**语义近似**，要显式裁决。

## 未做 / 边界

- 未实测真机注册表 / ini 的 `PresentInterval`（若真机不是 0，上面的裁决要降级成"取决于配置"）。
- 真机 >60 是否被 DWM / D3D9 排队限制（引擎外，需要 PresentMon 读数）—— 不在本仓判据面。
- `命令表 → 每帧毫秒` 的映射（哪些 case 是 30 / 10 ms）未逐条列。
## 裁决（2026-11-19，用户）

**按"用户不会改配置"处理**：`PresentationInterval` 只接受 **`IMMEDIATE (0x80000000)`**（引擎默认配置
`display:PresentInterval=0` 写的就是它）；**其它任何值 ⇒ 直接 abort 退出**（打一条带实际值的错误，
点名 `display:PresentInterval`），⛔ 不静默按 IMMEDIATE 继续。
理由：我们不实现"等 vblank"，静默放行等于**假装**尊重了 guest 的请求 —— 响亮停比默默背离好。
⇒ 本单从"要不要近似 vblank"降级为"**一个白名单判定 + 响亮停**"，不需要任何时序近似。
