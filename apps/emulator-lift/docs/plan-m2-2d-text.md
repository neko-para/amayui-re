# M2 · 2D 画面与文本（实现计划）

> **本文件是计划与判据，不是进度。** 做到哪一步看 `pnpm tools requirements plan`
> （本线 `REQ-01M6GJMENJ8XEK4ZPTA768YBPW`；M2 节点 `REQ-01M6H6G6DSRG1ZRD810Z0HTH8V`）。
> 上游： [`../README.md`](../README.md)（口径）· [`../SCOPE.md`](../SCOPE.md)（用户拍板的偏差）·
> [`../../../docs/03-emulator/platform-effort.md`](../../../docs/03-emulator/platform-effort.md) §3.2–§3.4 / §4（十簇与停手点）·
> [`plan-m1-cpu-base.md`](plan-m1-cpu-base.md)（M1 已交付的底座）。

## 1. 一句话

从 M1 的 L2（`_WinMain@16`）继续，把 **2D 表面 + 文本** 跑到**可机械判据的第一帧**：
先以 **headless 窗口 + 完整消息泵** 把真正的成本项（DirectDraw 表面 + GDI 字体/文本）量一遍，
再补真实 SDL 窗口 / 输入 / region；全程 **trace 驱动、虚拟时钟、两次运行逐字节/逐像素可比**。

★ **本文件只做工作量评估与切片，不设自动停手规则。** 后续是否做、做多少由用户决定；
本计划只回答"按这些切片，量级是多少、钱花在哪"。
★ 全量 M2 按上游口径约 **19–28 人周（4.4–6.5 人月）**；其中 headless 部分约 **13–19 人周**，
真实窗口/输入/region/标题约 **6–9 人周**。`platform-effort.md` §4 的"3–5 人月"停手点 A 只作为**参考坐标**，
本文件不据此自动做 go/no-go。

### 1.1 两个估算分组（不是阶段门）

| | 内容 | 估算（一个人，熟悉本仓） |
|---|---|---|
| **M2a · headless 主线** | 机制底座 · **headless 窗口 + 完整消息泵** · 启动解锁（图标 / 版本）· DDraw 核心 · GDI 字体/文本核心 · 第一帧/文本 + 帧哈希 | **13–19 人周**（3.0–4.4 人月） |
| **M2b · 真实呈现/交互** | **真实 SDL 窗口** / 输入 / region · 其余小族 · 标题/交互 smoke · 覆盖收口 | **+6–9 人周**（1.4–2.1 人月） |

★ **这些"人周"是上游口径的成本量级，不是 agent 的日历时间**：本仓的交付节奏可以比"人周"快得多，
但成本口径仍是判断"这条线值不值"的共同语言。真正的进度指标是**切片是否交付 + trace 撞到的新语义数**，
不是钟表。执行快只说明"打字不是瓶颈"，不说明"DDraw/GDI 的未知语义会变少"。
★ **启动顺序决定了"启动解锁"必须早做**：`LoadIconA` 与版本检查（`0x4BAF0A`）都在 `CreateWindowEx`
之前；版本检查若失败会 `MessageBox` 后直接退出（见 F6）⇒ 图标 / 版本 / 假窗口属于 P0。
★ **消息泵是 P0 的完整件，不是 M2b 的"最小假件"**（用户 2026-11-04 口径）：headless 只是**不建 SDL 窗口**，
`GetMessage` / `PeekMessage` / `PostMessage` / `SendMessage` / `Translate` / `Dispatch` / `Wait` / `WM_PAINT` /
`WM_TIMER` / `WM_QUIT` 的语义都要真做；P3 只把后端换成 SDL 并补输入/光标。

## 2. 现状（开工前必读）

### 2.1 M1 已经给了什么

- 执行源 / 解码 / 块缓存 / dispatcher / shim ABI / 句柄表 / guest 堆 / 虚拟时钟 / 文件与路径族 / 覆盖清单；
- 判据 A1–A5 与 CTest 分级；`_WinMain@16`（`0x4BA890`）可达。
- ★ 这些都是 M2 的**前提**，不是 M2 的内容。M2 不重做 CPU / 解码 / 七族。

### 2.2 继续执行的第一个卡点

```bash
cd apps/emulator-lift
./out/emulator-lift --game-dir ../../links/install --agerc ../../links/install/AGERC.DLL \
    --appdata ../../.tmp/appdata --max-insns 400000
# === unimplemented import (base gap) ===
#   gdi32!CreateICA ... 返回地址 = 0x0046512C
```

- **现查（2026-11-04，P0 之后）**：`LoadIconA`（`0x004BA9AA`）**已经不是卡点** —— P0 把 AGERC 真实镜像装进 guest
  （`0x30000000`）、资源 / 版本 / `_GetInstance@0` 从镜像解析、HICON 返回确定性假句柄，boot 越过它、
  注册完窗口类，停在 `gdi32!CreateICA`（`0x0046512C`，即 `sub_464FD0` 的"建屏幕 DC"）。
- ★ **2026-11-05 现查（已走得更远，以此为准）**：
  ```bash
  ./out/emulator-lift --game-dir ../../links/install --appdata ../../.tmp/appdata --max-insns 600000000
  ```
  已越过的站：`LoadIconA` → `CreateICA` → `SetCursor` → `shlwapi!PathRemoveFileSpecA` / `imagehlp!MakeSureDirectoryPathExists`
  → **`sub_4BA890` 的 RCVersion 检查**（曾卡在这里：`VerQueryValueA` 交回 UTF-16 指针 ⇒ `atoi` 读到 3 而不是 39）
  → `d3d9!Direct3DCreate9` → `d3dx9_43!D3DXCreateSprite` → 纹理 / D3DX 矩阵 → 现在停在
  **`IDirect3DDevice9::SetTransform(+0xB0)`**（fixed-function 流水线，属"新的大子系统"）。
  ⇒ 第一帧的闸门 = **D3D9 设备 + D3DX Sprite/纹理 + fixed-function 流水线**；既不是 DDraw、也不是 GDI 字体。
  ⚠ **P1（DDraw）不在本安装的启动路径上**（`set:CreateObject = 2`，见 §F2），但代码保留（另有一条不检查
  `CreateObject` 的服务分派入口可能调到它）。
- 历史口径保留：`0x4BA9A2 mov esi, LoadIconA` / `0x4BA9A8 call esi` 是**函数指针调用** ⇒
  census 的"LoadIconA 0 站点"是**下界**，不是"没调用"。**M2 必须 trace 驱动，不能按站点数排工作量。**
- `ShowCursor` 同理（`0x4BA082 mov esi, ShowCursor`）；M2 的输入簇会撞上它。
- ★ **跑 boot 的三个卫生要求**：`--appdata` 指向一次性临时目录（**跑前清空**：残留 `$$AGE.TMP` 会让引擎走
  "上次没正常退出 / exe 版本自检"的岔路）；⛔ 不许写安装树；⛔ 不传 `--agerc`（默认就取 `--game-dir/AGERC.DLL`）。

### 2.3 M2 的"完成分母"与"启动路径"不是同一个数

现查（开工前重跑；⚠ 不要把这两个数混成一个"进度百分比"）：

```bash
pnpm tools lift report          # 导入级：implemented / stub / bare
pnpm tools platform census      # 站点 / caller / vtable / 动态解析
```

- 导入表 310 条；M2 名下的显式 stub（**完成分母**）：
  `user32` 61 · `gdi32` 23 · `imm32` 1 · `version` 3 · `shell32` 1 · `shlwapi` 1 ·
  `imagehlp` 1 · `oleaut32` 3 · `ole32` 2（`CoCreateInstance` / `PropVariantClear`）= **96 条**。
- 这 96 条里，启动到第一帧**实际执行**的通常只是其中一部分（几十条）；其余在设置 / 菜单 / 存档路径上。
  ⇒ 实施顺序 = **trace 里先撞到哪条就先做哪条**；A5 的"M2 族内无 stub"是**收口**判据，不是开工判据。

**规模摘要（量级；站点数是下界，不是工作量）**

| 簇 | 量（现查） | 现状 | 对应切片 | 估算 |
|---|---|---|---|---|
| 机制 + headless 窗口/消息泵 + 启动解锁 | 5 个缺口（§2.4）+ USER32 窗口/消息 + 图标/版本 | 全无 / stub | P0 | 4–5 周 |
| DirectDraw 2D | 2 个动态入口 + 表面 vtable（Blt 95 / BltFast 55 / Flip 40 站点；⛔ 含其它 COM，不是纯 DDraw） | 0 | P1 | 4–6 周 |
| GDI 字体 / 文本 | 14 导入 / 310 站点 | 0 | P2 | 3–5 周 |
| 第一帧集成 | trace 驱动；1 分钟墙钟上限 | — | P5a | 2–3 周 |
| 真实 SDL 窗口 / 输入 | USER32 60 有调用 / 261 站点（消息语义已在 P0） | 1 实现（wsprintfA）+ 61 stub | P3 | 2–3 周 |
| region + 其余小族 | region 7/14；小族 ~9 导入 | 0 | P4 | 2–3 周 |
| 收口 / 标题 / 性能 | — | — | P5b/P6 | 2–3 周 |

### 2.4 五个必须先补的架构缺口（它们比"再写 30 个 API"更靠前）

| # | 缺口 | 现状 | 为什么 M2 第一周就会撞上 |
|---|---|---|---|
| G1 | **动态导出转发** | `GetProcAddress` 只特判 AGERC，其它一律 `127`（`kernel32_process.cpp`） | DDraw 就是 `LoadLibraryA("DDRAW.DLL")` + `GetProcAddress("DirectDrawCreate")`（`0x43AE30`） |
| G2 | **guest 可见的 COM 对象 / vtable** | 没有；现有只有 IAT 桩页 | 引擎用 `(*(…)(obj + off))(...)` 调 DDraw/Surface 方法（`+24` / `+20` / `+28` / `+44` / `+68` / `+100` / `+104` / `+128` …） |
| G3 | **可嵌套的 guest 回调栈** | 只有一个 `GuestCallbackState`（EnumSystemLocalesA） | 消息泵要调 guest WndProc；WndProc 里的 `SendMessageA` 又会再进去；DDraw `EnumDisplayModes` 与 GDI `EnumFontFamiliesExA` 也是 guest 回调 ⇒ 必须 LIFO 栈，不是单实例 |
| G4 | **guest 内存里的 surface + Lock 指针** | 没有 surface 分配器 | `Lock` 把像素指针交给 guest，缩略图按 pitch + 通道掩码直接读；宿主管家内存的指针不能给 guest |
| G5 | **伪造 AppData / 无头呈现** | `--game-dir` 是唯一根，且可写 | 安装树很大（本机 7.5 GB，随安装变）；游戏会写 `SYS4REG.INI` / 存档 / "上次异常终止"标记 ⇒ 需要一个 `--appdata` 把特殊目录引到临时目录（D5） |

★ G1–G3 是**纯机制**，可以先用自造 guest 代码做夹具（不需要游戏）；G4/G5 是**宿主设施**，同样 pure 档可测。
★ 这五条做完之前，不要开始数"实现了多少条导入"——那会把机制债伪装成进度。

### 2.5 可以复用的**知识**（不是代码）

- `apps/emulator-cxx/src/core/text_alpha.h`：GGO_GRAY4 = **17 级**、GGO_GRAY8 = **65 级**的量化口径。
- `apps/emulator-cxx/SCOPE.md` §1.1：宿主替换字体后的**基线 +2px** 实测值、20px/28px 的偏差记录。
- `apps/emulator-cxx/src/core/text_raster.cpp`：FT 取字 / 合成 / 两趟模式的**读法**（本线仍自己写平台 shim）。
- ⛔ 不复用 `apps/emulator-cxx` 的 core 当 shim（README §1 的边界）；它也没有 DDraw/Win32 形状层。

### 2.6 M1 的未收口债务里，哪些会直接影响 M2

| 债务 | 单 | 对 M2 的影响 |
|---|---|---|
| x87 / MMX·SSE 逐项复核未做 | `REQ-01M6K7PQK23EGJDJBQ6XKKE4HR` | 渲染 / 文本路径若执行到未实现形态会**响亮失败**；先分清"指令没实现"与"画面不对" |
| PE 载入对坏节/坏导入静默跳过 | `REQ-01M6JFZFCQ956CBFMKKQ37PCYK` | M2 要读 PE 资源目录，载入器的"半载入"会让资源解析指错 |
| 码页族三条质量缺陷 | `REQ-01M6K69JNGE7EP8ZNRDAXNSYZZ` / `…K28AT5…` / `…K9HM3…` | 文本输入 / 文件名转换的边界；M2 的主路径是 A 版字符串，影响有限但要记 |
| `Virtual*` 三处与文档冲突 | `REQ-01M6JJ6ZYNT6FP5ESHRTFG9HA4` | surface 分配若走 `VirtualAlloc` 会撞上；建议 surface 用**独立 arena**，不混进 `VirtualAlloc` |

## 3. 语料与结构事实（它们决定切法）

> 这一节的数字都可现查；引用时连命令一起给。⛔ 不把"某条 API 没被调用"当成"不需要实现"（见 F2）。

### F1 · 启动到第一帧的顺序（`.lst` 可读）

`_WinMain@16` 的窗口段（`0x4BA963` 起）大致是：

```
LoadIconA（×2，函数指针）→ LoadCursorA → GetStockObject(BLACK_BRUSH)
→ RegisterClassExA → CreateWindowExA（sub_4BA310）→ ShowWindow → UpdateWindow
→ 引擎 init（配置 / AGERC / 版本 / 文件）
→ DDraw init（sub_406730 → sub_43AE30 → sub_440A20）
→ 主循环 sub_412290：PeekMessageA(PM_NOREMOVE) → GetMessageA → TranslateMessage → DispatchMessageA
```

- `sub_4BA310` 用 `GetSystemMetrics(7/8/15/4)` 算外层尺寸，`CreateWindowExA` 传的是**外层**宽高；
  真实 Windows 的客户区 = 外层 − 边框 − 标题栏。⇒ shim 要么按同样的非客户区规则扣，要么 `GetClientRect`
  会与引擎算出的设计尺寸不一致（后续 DDraw 主表面尺寸跟着错）。
- `sub_412290` 的 `PeekMessageA` 的 `wRemoveMsg = 0`（**PM_NOREMOVE**），然后才 `GetMessageA` 取走。
  ⇒ shim 必须实现"看一眼不移除"的语义；只实现 `GetMessage` 会让循环每次多取一条/丢一条。
- 主循环用 `timeGetTime` 做帧节奏、`Sleep` 让出时间片；M1 的 `Sleep` 已推进虚拟时钟。

### F2 · 显示初始化有**两条路线**；本安装走 **D3D9**（`set:CreateObject` 实测 = **2**）

`sub_406730` 按 `set:CreateObject` 的**两位掩码**各走一套初始化；两段是**顺序 if，不是互斥**
（值为 3 时两段都会跑）：

| 位 | 值 | 判定 EA | 块内做什么 |
|---|---|---|---|
| bit0 | `1` | `0x406751 test al,1` → `0x406753 jz loc_4068A9` | **DDraw**：`sub_43AE30`（`0x40675F`，装载 `DDRAW.DLL` + 取 `DirectDrawCreate`）→ `sub_440A20`（`0x406805`，建 DDraw 对象）→ `sub_450B50`（`0x40685E`）；任一步失败 ⇒ 弹对应错误串 + `return 0` |
| bit1 | `2` | `0x4068BF test al,2` → `0x4068C1 jz loc_406C3F` | **D3D9**：`sub_498CC0`（`0x406ADA`，`Direct3DCreate9` + `CreateDevice`）→ `sub_4A6EE0`（`0x406B37`，把渲染器挂到设备上）；失败 ⇒ 弹「DirectX…」+ `return 0` |

- **不互斥**：bit0 的块没有收尾跳转，控制流**落在** `loc_4068A9`（bit1 的判定）⇒ 值 3 会先 DDraw 再 D3D9。
- **位只有两位（就 `sub_406730` 与引擎自己的写值而言）**：全 `.lst` 共 **32** 处
  `push offset "set:CreateObject"` —— 1 处是写入（`0x494FAE`，`Reg::set`），其余 31 处是读取。
  这 31 处读取的用法只有四种：`test al,1`、`test al,2`、`and eax, (1<<set:DrawMode)` 后 `cmp`、
  当 `sprintf` 的参数（`0x4178A6` 的「CreateObject=0x%x」）—— **没有一处判 `& 4` / `& 8`**。
  ⇒ bit0 = DDraw、bit1 = D3D9；引擎自己写这个值时只会写 **3**（位 0|1）。
- `set:DrawMode` 决定"当前是哪一位"：`sub_417800` 在 `0x4178F9` 算 `1 << set:DrawMode`，若
  `(CreateObject & mask) != mask` 就**把 CreateObject 强制改成 3**（`0x417908 push 3` → `0x417915` `Reg::set`）
  并打「DirectX オブジェクト作成設定が不正です」。
- 两段都不过时不报错：直接走到公共尾巴 `loc_406C3F`（只记 `display:ScreenMode` 到 `this+0xA40D8`）并 `return 1`。

**实测值：`CREATEOBJECT` = `"2"`、`DRAWMODE` = `"1"`** ⇒ bit0 = 0、bit1 = 1 ⇒ **本安装只走 D3D9**。
`1 << 1 = 2` 与 `CreateObject = 2` 相等 ⇒ `sub_417800` 的强制改写**不触发**。

- 真源是随包初始化文件，不是我们的代码：`repo:links/install/SYS4INI.BIN`（446,558 B，
  sha256 `b6738009b593dab650db5b8e06603c3d29efe822486c7c818edbb9c7b5c9ee1f`）是 S4IC 容器
  （`[0,300)` 头 + `[300,312)` 三个 uint32 `1,692,004 / 1,692,004 / 446,246` + LZSS 体，`sub_436A80`：
  `N=4096 / F=18 / 阈值 2 / r0 = N-F`）⇒ 解出 **1,692,004 B** = 21,109 条 ALF TOC + 尾部初始化块。
- 初始化块在**解压载荷偏移 1,691,024**：`uint32 953`、`uint32 51`（键值对数）、随后 51 对 `key\0value\0`；
  `CREATEOBJECT` 的键字节在偏移 **1,691,032**，紧跟的值是 `"2"`（`DRAWMODE` 的值是 `"1"`）。
- 读法链（EA）：`_WinMain` `0x4BAB1A push "SYS4INI.BIN"` → `0x4BAB26 call sub_414AC0`
  （trace 里 `CreateFileA` 的返回地址 `0x414B1F`）→ Reg 装载 `sub_494220`（vtable 分派，**无直接 call**）
  → `0x494F92 __stricmp("CREATEOBJECT")` → `0x494F9F _atoi` → `0x494FBC sub_434D00("set:CreateObject", &v)`。

**怎么重跑（只看运行期，不读代码）**

```bash
cd apps/emulator-lift
G=../../links/install
# ① bit1 块的第一条指令：能停住 ⇒ bit1 = 1
./out/emulator-lift --game-dir $G --agerc $G/AGERC.DLL --appdata ../../.tmp/appdata-probe \
  --until ea 0x4068C7
#   期望：stop      : until-ea @ eip=004068C7      （本机 insns = 102,914,704）
# ② bit0 块的第一条指令：停不到 ⇒ bit0 = 0，一路撞到 d3d9 的未实现导入
./out/emulator-lift ... --until ea 0x406759 --max-insns 120000000
#   期望：stop      : unimplemented @ eip=7FFE00AC（d3d9!Direct3DCreate9），insns = 102,919,944
# ③ ini 的键比较真的发生在启动路径上（而不是"文件里写着但没被读"）
./out/emulator-lift ... --until ea 0x494F92
#   期望：stop      : until-ea @ eip=00494F92      （本机 insns = 35,853,538）
# ④ 全程没有 DDraw 装载
./out/emulator-lift ... --trace-shims --max-insns 120000000 | grep -i ddraw   # 空
```

- ★ 三条 `--until ea` 是**运行期**证据；`--trace-shims` 全程只有
  `LoadLibraryA` 的 `AGERC.DLL` / `KERNEL32.DLL` / `Shell32.dll` / `dwmapi.dll`（+ `GetProcAddress` 若干），
  **没有** `DDRAW.DLL`、也没有 `DirectDrawCreate`。
- ★ 数值的**独立**来源是上面那次 S4IC 解压：解出的字节里 `CREATEOBJECT\0 2 \0` 可逐字节核对
  （算法照 `sub_436A80` 的 `.lst` 逐字实现，脚本是一次性的、⛔ 不入库）。
- ★ 调用链（trap 现场栈回溯）：`0x498D14 ⇐ 0x406ADF ⇐ 0x417C20 ⇐ 0x4BB412 ⇐ 0x4EDB8B`
  = `sub_498CC0 ⇐ sub_406730 ⇐ sub_417800 ⇐ _WinMain@16 ⇐ CRT`。

#### F2.1 · DDraw 仍是**动态**入口（G1 的直接证据；本安装的启动路径不经过它）

```
0x43AE30（调用点 0x40675F / 0x406502）: LoadLibraryA("DDRAW.DLL") → GetProcAddress("DirectDrawCreate") → 存 [this+0FA8]
0x43C380（调用点 0x440A6D / 0x406518）: DirectDrawCreate(...) → QueryInterface(IID_IDirectDraw2) → Release(原对象)
0x440A20（唯一调用点 0x406805）: GetAvailableVidMem(+0x5C) → GetDisplayMode(+0x30) → SetCooperativeLevel → ...
```

⇒ `DirectDrawCreate` 不能只返回一个裸对象：`QueryInterface(IID_IDirectDraw2)` 必须成功（错误串
「IDirectDraw2の作成に失敗しました」就是它），且原对象要被 `Release` 掉。

- 当前 `GetProcAddress` 返回 `127` ⇒ **这条路径在 M2 第一步就断**。
- 动态解析站点还有：`Shell32!SHGetFolderPathA`（4 处）、`USER32!RegisterTouchWindow` / `UnregisterTouchWindow` /
  `SetGestureConfig` / `GetTouchInputInfo` / `GetGestureInfo` / `CloseGestureInfoHandle` / `CloseTouchInputHandle`、
  `KERNEL32!IsDebuggerPresent` / `SetThreadExecutionState`、`COMCTL32!_TrackMouseEvent`。
  ⇒ **导出转发必须是一张表（module!name → 我们自己的 thunk），不是给 DDraw 开的特例。**

#### F2.2 · DDraw 在本安装的启动路径上是 **dead**

**结论：是（dead）—— 但只对"本安装的启动路径"成立，不是"二进制里不可达"。**

- `sub_440A20`（DDraw 的**完整**初始化：`GetAvailableVidMem` / `GetDisplayMode` / `SetCooperativeLevel`）
  **只有 1 个调用点 `0x406805`**，就在 bit0 块里。
- `sub_43C380`（`DirectDrawCreate` 的封装）只有 2 个调用点：`0x440A6D`（在 `sub_440A20` 里）、
  `0x406518`（在 `sub_4764B0` 的 chunk `0x4064E0` 里 —— 见下）。
- `sub_43AE30`（`LoadLibraryA("DDRAW.DLL")` + `GetProcAddress`）只有 2 个调用点：`0x40675F`（bit0 块）、
  `0x406502`（同一个 chunk）。`0x4064E0` 这个 chunk 是 `sub_4764B0` 的体外块（`0x4764B6 jmp 0x4064E0`），
  **它不检查 `CreateObject`**，一进来就装载 DDraw 并 `DirectDrawCreate`。
- 运行期三条证据（§F2 的 ①②④）都指向 bit0 = 0 且没有发生任何 DDRAW 装载 ⇒ 上述两条入口在 boot 上都没被走到。

⚠ **未测（不许写成"DDraw 可以删掉"）**：`sub_4764B0` 在**全 `.lst` 里没有直接 `call`**
（`disasm-fn callers sub_4764B0` = 0 个调用方 + 1 处取址证据：表 `0x526B70` 的首项；
`0x4B5338` 处出现服务名字符串 `"AGE:IAGEService"`）⇒ 它是**动态分派**入口。
它是否会在**别的路径**（脚本 / 服务调用 / 换分辨率）被调到 = **未测**。

### F3 · DirectDraw 的形状与用量（= **另一条渲染后端**；本安装的启动路径不经过它）

> ★ 定位（2026-11-05 取证，见 §F2）：本节是 **DDraw 后端**的形状。本安装 `set:CreateObject = 2`
> ⇒ 启动路径走 D3D9，**这整节在 boot 上不发生**。它仍是同一条线上的另一条后端（P1 已交付），
> ⛔ 不是"没用"；只是**不能拿它当第一帧的前置**。

- 创建表面：`sub_43C8D0` 填 `DDSURFACEDESC`（`dwSize = 108`，`dwFlags = 7` = CAPS|HEIGHT|WIDTH），
  按 `a6` 写三档 caps（`0x840` 系统内存 / `0x4040` 显存 / `0x40` 离屏平面），再走 vtable **+24** `CreateSurface`；
  随后 `QueryInterface(IID_IDirectDrawSurface3)`。
- **枚举显示模式**：`sub_43C470` 走 vtable **+0x20** `EnumDisplayModes`，回调是 guest 的 `sub_43BEA0`；
  枚举失败会打印「ビデオモードの列挙に失敗しました」并返回 0。⇒ DDraw 必须**至少回调目标模式**
  （1280×720×32 / ×16），并复用 G3 的 guest 回调机制；返回"零个模式"不是成功。
  ★ **回调返回值的方向已按取证钉死（2026-11-04）**：**0 = `DDENUMRET_CANCEL`（停止），非 0 = 继续**。
  取证：引擎回调链 `0x43BEA0 → 0x43AFE0` 的**每个**返回出口都是 `mov eax,1; retn 8`（`0x43B024` / `0x43B061`）
  ⇒ 引擎永远"继续"。⇒ ⛔ 不要写成"非 0 提前终止"：那会让枚举在第一条模式后就停、
  把目标分辨率判成不可用。实现见 `src/shims/ddraw.cpp`，夹具 `tests/ddraw.cpp` 的两个方向都覆盖。
- 方法序（`platform-effort.md` §3.4；⚠ 只有 `+68` / `+104` 有错误串直接作证，其余是按 DDraw 方法序推测）：
  `Blt +20` / `BltFast +28` / `Flip +44` / `SetCooperativeLevel +80` / `SetDisplayMode +84` /
  `Lock +100` / `ReleaseDC +104` / `GetDC +68` / `Unlock +128`。
- **颜色键是显式设置**：`sub_43B070` 走表面 vtable **+0x74（116）`SetColorKey`**，`flags = 8`（`DDCKEY_SRCBLT`）；
  `sub_43B0E0` 先把 RGB 按表面像素格式的**通道掩码**转成目标格式再设键。⇒ `GetDisplayMode` / `GetSurfaceDesc`
  返回的 masks 必须与 Blt 的像素布局一致，否则透明处会变黑框（或整片被键掉）。
- 像素路径：**存档缩略图**要"锁定表面 → 像素 + pitch + 通道掩码"（`data/spec/chapters/65-save.md`）；
  **文本**走"表面 DC"（`sub_43B460`，错误串「GetDC エラー…」）⇒ GDI 必须能往同一块 surface 像素上画。
- 显示格式：引擎的 `display:ScreenMode` / `display:FullScreenBit` 会选择 16 / 32bpp 路径；
  **GetDisplayMode 返回什么，引擎就按什么走**。⇒ 见 D4：内部固定一种格式、另一种也要能拒/接受。

### F4 · GDI 字体与文本

- GDI32 23 导入 / 367 站点：字体/文本 14 / 310，region 7 / 14（`platform-effort.md` §3.3）。
- 引擎**自己合成字形**：`fuFormat = 5`（GRAY4，17 级）/ `6`（GRAY8，65 级）/ 否则 `1`（1 位），
  调 `GetGlyphOutlineA` 拿位图，自己写进纹理；`TextOutA` 只在旧路径（10 站点）。
- **两个 DC**：度量在 `CreateICA("DISPLAY")` 的屏幕 DC（`sub_464FD0`，存在 `this+1108`）；
  绘制在 DDraw 表面 DC（`this+1104`，`sub_43B460`）⇒ **字体簇不能先于 DDraw**。
- 字面名是「ＭＳ ゴシック」（`asc_52686C`，引用点 `0x456D09`）；`0x417AC3` 还会把某个字体文件读进内存再
  `AddFontMemResourceEx` —— 在 macOS 上这个文件（系统字体）不存在 ⇒ **必须有宿主字体回退**（见 D3）。
- GGO 位图的**行对齐是 DWORD**；`gmBlackBoxX/Y`、`gmptGlyphOrigin`、`gmCellIncX` 都会被引擎读。
  ⇒ 量化与对齐错一位，字就会花/错位，而 `GetTextExtentPoint32A` 可能仍然"看起来对"。

### F5 · 窗口 / 消息 / 输入 / region

- USER32 62 导入 / 60 有调用（census 的 LoadIconA/ShowCursor 是 0，但**实际通过函数指针调用**）。
- WndProc `sub_4B92F0` 是一个大 switch：至少含 `WM_DESTROY` / `WM_MOVE` / `WM_ACTIVATE` / `WM_SETFOCUS` /
  `WM_PAINT` / `WM_CLOSE` / `WM_ERASEBKGND` / `WM_SETCURSOR` / `WM_GETMINMAXINFO` / `WM_KEYDOWN` / `WM_SYSKEYDOWN`，
  以及 `0x200+`（鼠标）与 `0x400+` 的几段；**准确集合由 M2 的窗口夹具枚举，不靠记忆抄**。
- 主循环的 `PeekMessage(PM_NOREMOVE)` / `GetMessage` / `Translate` / `Dispatch` 是**唯一**的泵；
  另有 `WaitMessage` / `IsDialogMessageA` / `MsgWaitForMultipleObjects` 的辅助路径（后者可能不可达）。
- 输入是**两套并存**：`GetAsyncKeyState`（5 站点，轮询）+ `WM_*`（推送）。M2 的 shim 必须同时维护
  "SDL 事件的影子键盘状态"与"消息队列"，否则标题/ADV 的点击会一半灵一半不灵。
- region：`CreateRectRgn` / `CreateEllipticRgnIndirect` / `CreatePolygonRgn` / `CombineRgn` / `GetRegionData` /
  `GetRgnBox` / `PtInRegion`（GDI 7 条）+ `GetUpdateRgn` / `ValidateRgn`（USER32）⇒ 命中测试与重绘区。

### F6 · 资源 / 小族 / 版本 / AGERC

- `LoadIconA` 要读 PE 资源目录（`RT_GROUP_ICON` / `RT_ICON`）；`version` 3 条要读 `VS_VERSIONINFO`。
  `FindResourceA` / `SizeofResource` / `LoadResource` / `LockResource` **不在导入表、`.lst` 也没有调用点**。
- **AGERC 的镜像来源只有两段**（用户 2026-11-05 裁决）：① `--agerc <path>`（显式覆盖）；
  ② `--game-dir/AGERC.DLL`；**拿不到 ⇒ `LoadLibraryA` 返回 0 + 126**（⛔ 不假装成功、⛔ 不回退 `DATA1.ALF`）。
  引擎本身也不回退：它是**按名字** `LoadLibraryA("AGERC.DLL")` 从**应用目录**取（`.lst` `0x48E74B`，
  字面量 `.data:005292D8`）。取证与数字见 [`agerc-image.md`](agerc-image.md)：
  - `repo:links/install/AGERC.DLL`（`6241de66…`，848,896 B）= **汉化版 `DATA1.ALF` 里那份被抽出来的免壳件**
    —— 7 节 / 21 导出 / **40 条资源全部可映射** / `FileVersion 39.1.0.0`。★ **本线默认就用它**：
    引擎自己也是按名字从应用目录取，取到的就是这一份。
  - `DATA1.ALF` 的 `SYS4INI.BIN` 第 21072 条（`f48d1c0f…`）= 同一 build 的**未汉化**件：`.text` / `.data` / `.reloc`
    与上面那份逐字节相同，只有 **10 条 MENU/DIALOG 资源**不同（图标 / 版本资源逐字节相同）。
    ⇒ **它不是装载来源，只是取证对照**（"免壳"这个结论的证据，也是与正版加壳件对照的材料）；
    ⛔ 不为它写读取分支（用它还会读到未汉化的菜单 / 对话框文案）。
  - ⛔ **不需要拆壳件**：这两份都是免壳 PE。只有**正版**安装树的根件（`repo:links/raw/AGERC.DLL`，AsProtect 加壳，
    335,872 B）才资源不可读（26/40 条无文件字节）—— 那是"正版树拿不到可映射根件 ⇒ 0 + 126"那一档，
    不是 AGERC 的通性（需要时用 `--agerc` 显式指一份免壳件）。
  - ⛔ 伪 HICON **不是预期路径**：只有 `.rsrc` 映射真的失败时才允许，并且必须打日志。
- 小族：`SHGetSpecialFolderPathA` 5 站点（我的文档 / AppData）· `PathRemoveFileSpecA` 3 · `MakeSureDirectoryPathExists` 3 ·
  `ImmAssociateContext` 1 · COM/VARIANT 5。它们归 M2，但**不一定在启动主路径上**。
- 存档根：`data/spec/chapters/65-save.md` 写明 `set:UseAppDataFolder == 1` 时根 = 用户应用数据目录，
  否则 = 安装目录。⇒ `SHGetSpecialFolderPathA` 的返回值必须能被 VFS 映射；否则第一次存档/读配置就会"文件不存在"。
- `WinMain` 里有一次「上次程序没有正常结束」的检测（`sub_4BA6B0`）：若标记存在，`MessageBoxA` 返回 IDOK
  后**直接退出**。⇒ 测试被强杀后必须清理这个标记，否则下一次 boot 会"什么都没做就退出"（以及写进安装树）。
- 主循环退出走 `_CxxThrowException`（`0x4123C4`）⇒ 按 `SCOPE.md` §1 这是**硬错误**；
  M2 的测试必须用 `--max-frames` 主动停，不要靠 `WM_QUIT` 收尾。

### F7 · 性能（这条可能比"能不能画出来"更早决定路线）

- 现查：`./out/lift-bench` 在 macOS arm64 上约 **12.9 MIPS**（纯整数、自造字节）。
- 60fps 的预算是 16.6ms ⇒ 13 MIPS 下约 **21 万条指令/帧**。本作一帧的引擎 + 渲染代码很可能高于这个量级。
- ⇒ M2a 必须同时报："第一帧的指令数 / 墙钟时间 / 连续 N 帧的帧时间"，而不是只报"画出来了"。
  若稳定态 < 10fps，**优化（线程化解释器 / 热块）是一条单独的单**，不要混进 M2 的功能判定。

## 4. 决策点（推荐值 + 否决的代价）

> 和 M1 的 D1–D6 一样：每条都写"推荐值"和"不这么做的代价"。★ 标 **〔需用户确认〕** 的会改 `SCOPE.md` 或路线。

### D1 · 先做 headless 窗口（★ 已定 2026-11-04）

**用户口径**：先实现 headless 窗口，但**消息泵系统必须完整**；真实 SDL 窗口/输入后做。
- "headless" 只意味着 **不建 SDL 窗口 / 不读真实显示器**；窗口句柄、类表、客户区、消息队列、
  `WM_CREATE` / `WM_SIZE` / `WM_PAINT` / `WM_TIMER` / `WM_QUIT`、`SendMessage` 同步重入、`PostMessage` 异步、
  `PeekMessage(PM_NOREMOVE)` 的"看一眼不移除" —— 全部按 Win32 语义真做。
- `CreateWindowExA` 在 headless 下只产生 guest 句柄 + 消息，不调 SDL；DDraw `Flip` 落到宿主帧缓冲 + 哈希。
- ★ P3 只换**后端**（SDL 窗口 / 事件 / 光标）并补输入；**不改** P0 建立的窗口/消息语义；P0 的窗口/消息夹具在 P3 之后仍要绿。
- 代价：M2a 需要先把消息泵做完整（P0 4–5 周，而不是 2–3 周）；换来的是 DDraw/GDI 的测量不被窗口/输入噪声干扰。

### D2 · 呈现：headless 帧缓冲 + 可选 SDL 窗口

**推荐：把"present"做成一个接口，两个后端**：
- `offscreen`（默认测试）：DDraw `Flip` 把 primary 拷到宿主缓冲；`--dump-frame` / `--frame-hash`；
- `sdl`（交互）：同一份像素上传 SDL 纹理；窗口尺寸固定 1280×720（可缩放）。
- 代价：两条呈现路径要各测一遍；但 headless 是确定性的唯一可靠办法。

### D3 · 字体：宿主 Amayui CN 替换 + 度量补偿（★ 已定 2026-11-04：照 emulator-cxx 策略）

**用户口径**：参考 `apps/emulator-cxx` 的已有策略处理。**落地**：
- 默认按**宿主族名 `Amayui CN`** 取字（`corpus/assets/fonts/Amayui-CN_cnjp.ttf`，粗体同目录 `-Bold.ttf`），
  **忽略**引擎字面名「ＭＳ ゴシック」；`--font-engine-name` 保留 A/B 对照档。
- 与 emulator-cxx 一致：`EMULATOR_CXX_*` 那套"总是 Amayui CN / 基线 +2px / 字重走 Bold / 同色单趟异色两趟"的策略
  在 lift 里按同一口径实现；lift 的参数名用 `--font` / `--baseline-lift` / `--font-engine-name`，不照抄环境变量名。
- `AddFontMemResourceEx` 若真的收到引擎读进来的字体内存：**仍以宿主替换为准**（不因它改字体来源）；
  引擎字面名的 A/B 档只用于对照。
- **基线补偿**：Amayui 的 ascent 比 MS Gothic 大（emulator-cxx 实测 20px 20 / 28px 28 vs 17 / 24），
  宿主侧默认抬 **2px**（`--baseline-lift` 可调，0 = 关）；这是**宿主视觉策略**，⛔ 不是引擎语义。
- **为什么**：macOS 上没有 MS Gothic；跨平台要可复现；本仓的译文就是为 Amayui CN 排的。
- **代价**：与真机相比行高 / 基线可能差 1–2px（**登记为平台偏差**，不写进 spec）；
  文本宽度会随字体变，排版要按"自洽"判据（见 B5），⛔ 不按"与真机逐像素相同"判。

### D4 · 显示格式：内部 32bpp，接受 16bpp 请求

**推荐**：`GetDisplayMode` 默认返回 **32bpp（8888，masks 00FF0000 / 0000FF00 / 000000FF）**，pitch = width×4；
若引擎的 `SetDisplayMode` 明确要 16bpp，则切到 **565**（F800 / 07E0 / 001F）并让 Blt/DC 走 16bpp 分支。
- 先用 32bpp 打通，是因为 GDI/DDraw 的 32bpp 分支最直；16bpp 只做引擎实际请求的那条。
- **不做**：palette / 8bpp / 24bpp（除非 trace 证明启动路径需要）。
- 代价：如果引擎只走 16bpp（配置里写死），32bpp 的"默认"没用；P1 仍要把 565 的 Blt/Lock/DC 做对。

### D5 · 伪造 AppData：新增 `--appdata <host-path>`（★ 已定 2026-11-04）

**用户口径**：需要一个**传入伪造 AppData 的路径**。**落地**：
- 新 CLI：`--appdata <host-path>`。它只影响**特殊目录**：`SHGetSpecialFolderPathA` 的
  `CSIDL_APPDATA` / `CSIDL_LOCAL_APPDATA` / `CSIDL_PERSONAL`（以及 `GetTempPath` 若走同一 helper）
  都映射到该宿主目录下的对应子目录（`Roaming` / `Local` / `Documents` 之类的形态由实现固定并写日志）。
  返回给 guest 的仍是**Windows 形状的 guest 路径**（例：`C:\Users\Amayui\AppData\Roaming`），
  VFS 再把它映射回 `--appdata` 指向的宿主目录。
- 测试用临时目录传入 `--appdata` ⇒ 存档 / 配置写到临时目录；安装树 `--game-dir` 不因 AppData 被写。
- **不引入**通用 `--work-dir` 写覆盖（用户只要求 AppData 这一条）：引擎若仍把某些文件写到 `--game-dir`
  的相对路径，测试必须显式备份/恢复那些文件（或指向一次性最小副本），并在测试里断言安装树未被改动。
- 代价：~0.5–1 周；需要把"特殊目录 → 宿主目录"的映射集中在一处，⛔ 不把宿主路径漏给 guest。

### D6 · 资源 / 版本：实现内部 PE 资源读取；删掉 4 条无人调用的注册

★ 这一条属于 **P0 的启动解锁**，不是 P4：`LoadIconA` 与版本检查都在 `CreateWindowEx` 之前；
版本不符时 `MessageBox` 后直接退出（F6），会挡住整个 M2a。

**用户口径（2026-11-04，更新）**：直接读 **AGE.EXE** 的资源；**AGERC 同样按真实模型装载**——
把 AGERC.DLL 的 PE 节映射进 guest 内存，用**真实 ImageBase 作为 HINSTANCE**，资源 / 版本从该镜像解析。
⛔ 不再用“假实例 / 假句柄 / 编造资源”。AGERC 的三个导出仍按 `SCOPE.md` §1 的模态 no-op stub 执行
（用户没有要求执行 AGERC 的对话框代码）；`_GetInstance@0` 返回真实模块基址。
- 在 `runtime/` 加 `PeResources`（解析 `.rsrc`：`RT_ICON` / `RT_GROUP_ICON` / `VS_VERSIONINFO`），
  对 AGE.EXE 与 AGERC.DLL 两个 `PeImage` 都可用；
- `mapPeImageAt()`：AGERC 映射到不冲突的 guest base（优先 ImageBase，冲突则 0x30000000）。
- **镜像来源只有两段（用户 2026-11-05 裁决）**：① `--agerc <path>`（显式覆盖）；
  ② `--game-dir/AGERC.DLL`（走 VFS 解析，大小写不敏感）；**都拿不到 ⇒ `LoadLibraryA` 返回 0 + 126、
  `GetProcAddress` 同样 126，⛔ 不假装成功、⛔ 不回退 `DATA1.ALF` 条目**（引擎本身也不回退：它按名字
  `LoadLibraryA("AGERC.DLL")` 从应用目录取，见下面"为什么"）。
  ★ 当前测试安装树 `repo:links/install` 的根件就是免壳件（848,896 B）⇒ 走②，**P0 不需要 `--agerc`**。
  ★ 正版树的根件是 AsProtect 加壳件（`repo:links/raw/AGERC.DLL`，335,872 B，资源不可映射）⇒ 走②拿不到，
  **就停在 0 + 126**；需要时用 `--agerc` 显式指一份免壳件（`DATA1.ALF` 的条目只作取证对照，不是装载来源）。
  ★ ②那份是**汉化过的**（10 条 MENU/DIALOG 资源被替换）⇒ 菜单 / 对话框文案是中文；这是默认路径的预期行为。
- `LoadIconA`：`hInstance == AGE 主模块` ⇒ AGE 资源；`hInstance == agerc_base` ⇒ AGERC 资源（`IDI_ICON1`）；
  从镜像的 `RT_GROUP_ICON` → `RT_ICON` 拼图标。窗口图标在 headless 下不被绘制（无 `WM_SETICON` /
  `SetClassLong`，只有 `WNDCLASSEXA.hIcon` → `RegisterClassExA`）。**免壳件的 40/40 资源可映射 ⇒ 不需要拆壳件**；
  伪 HICON 只作 `.rsrc` 数据项映射失败时的**应急**（loud log；平台偏差，`SCOPE.md` §1），⛔ 不是预期路径。
- `version!GetFileVersionInfo*`：AGE 用 `cpu.image`；AGERC 用 `cpu.shim.agerc_image`（真实镜像），
  支持 `\` 与 `\StringFileInfo\<langcp>\<Field>`；不再对 AGERC 路径 hard error。
- `FindResourceA` / `SizeofResource` / `LoadResource` / `LockResource` 这 4 条**删掉注册**（既不在导入表、
  也无人调用；要用等真需要时再加），走 `pnpm tools lift sync --write`。
- **为什么**：引擎是**按名字** `LoadLibraryA("AGERC.DLL")`（`.lst` `0x48E74B`，字面量 `.data:005292D8`），
  真机加载的是**应用目录**那份；三个导出由 `0x48E7A2` / `0x48E7B4` / `0x48E7C6` 取到 `[RCDLL+8/0C/10]`，
  `_GetInstance@0` 返回 AGERC 自己的 `hInstance`（`DllMain` 写入，EA `0x10034CD5`；导出桩 `0x10033121`
  是 5 字节 `jmp 0x10034D50`）。`LoadIconA` 的 `hInstance` 就是它（`0x4BA99C` → `0x4BA9A1 push eax`），
  而 `"IDI_ICON1"` 只在 AGERC.DLL 里（AGE.EXE 只有数字 ID 138）⇒ 只有真实装载 AGERC 才能过这一关。
  版本比较在 `0x4BAF26`：`set:RCVersion` 对 `[RCDLL+0x14]`（= AGERC `FileVersion` 经 `atoi`，`0x48E6FF`）。
- 代价：+0.5–1 周（模块映射 / 两段来源 / HINSTANCE 统一）。★ **本机就有免壳件**：ALF 条目 =
  `corpus/assets.json` 的 `binary/agerc-debug-unpacked`（`.lst` 语料的真前身），安装树根件是它的汉化派生
  ⇒ 图标 / 版本可在本机验，不必等 Windows 设备；未测的只剩"真机改名实验"与"AsProtect 运行期解包镜像"，
  见 [`agerc-image.md`](agerc-image.md) §8。

### D7 · region / 输入：放 M2b，M2a 只保留"退出键"

**推荐**：M2a 不做 region、不做完整键盘/鼠标；用 `--time-limit 60`（探索）/ `--max-frames`（确定性）主动停。
M2b 再做 region + SDL 输入 + 光标 + `GetAsyncKeyState` 影子状态。
- **为什么**：第一帧不需要命中测试；region 是 1–1.5 周、输入是 1 周，放在第一帧之后更稳。
- 代价：M2a 不能"点进标题"；它只证明"能画出第一帧 + 文本"。

### D8 · 工作量评估口径（★ 已定 2026-11-04：不设自动停手规则）

**用户口径**：目前只完成整个工作量的评估；后续是否做、做多少由用户决定，不需要额外策略。
- 本计划**不设**"超 3 人月就停 / 超 18 周就回退"的自动规则；`platform-effort.md` §4 的 3–5 人月只作参考坐标。
- 仍要记录：每个切片的**实际日历投入**、DDraw/GDI 撞到的**未预期语义数**、第一帧的**性能数字**；
  这些是用户后续决策的输入，不是自动闸门。
- 切片顺序（P0 → P1 → P2 → P5a → P3/P4 → P5b/P6）仍按依赖给；是否做到哪一步由用户拍板。

## 5. 验收（M2a / M2b 怎么算完成）

> 判据分档照 `AGENTS.md` §5：CTest 顶层无条件注册 = `pure`；落在 `if(EXISTS …)` = `assets`。
> 每条都要能在"改坏一处"时红，⛔ 不要恒真断言。

| # | 判据 | 档 | 属于 |
|---|---|---|---|
| **B1** | **机制**：`LoadLibraryA` + `GetProcAddress` 返回的 thunk 真的能调；guest `call [vtbl+off]` 能把 `this` 与参数送到 shim；guest→shim→guest→shim 的嵌套回调栈平衡、返回值正确 | pure | M2a |
| **B2** | **DDraw**：surface 在 guest 内存；`CreateSurface` / `BltFast`（`SetColorKey` 的键 + 越界裁剪）/ `Blt`（KEYSRC + WAIT）/ `Lock`+`Unlock`（pitch / 通道掩码往返）/ `GetDC`+`ReleaseDC`（文本画进同一块像素）/ `Flip`（present）/ `EnumDisplayModes`（guest 回调收到至少一条模式）各有夹具；两次运行帧哈希相同 | pure | M2a |
| **B3** | **GDI 字体/文本**：字体对象 / DC / `SelectObject` 恢复；`GetTextMetricsA`；GGO 1 位 / 17 级 / 65 级的**行对齐 + 值域**；`GetTextExtentPoint32A` 宽 = `GetGlyphOutlineA` 的 advance 之和 = 实际绘制宽度；`TextOutA` 的 bk mode | pure | M2a |
| **B4** | **第一帧**（assets）：headless 跑到 `Flip >= 1`；**探索运行**上限 1 分钟墙钟（`--time-limit 60`）；trace 里出现 `Blt`/`BltFast`；**没有执行到的未实现导入**（扩展现有 A5）；**确定性比对**用同一输入 + 固定 `--max-insns`/`--max-frames`，两次同一帧哈希相同；到点/没到都打印卡点 EA 与帧数并红 | assets | M2a |
| **B5** | **文本自洽**（assets）：探索上限内出现 `GetGlyphOutlineA`；一段截获的字符串满足"extent = Σ advance = 实际像素带宽度"；文本像素带非空且两次运行相同（确定性比对同上） | assets | M2a |
| **B6** | **窗口/消息（完整泵）**（pure）：假 WndProc 记录消息序列；`CreateWindowEx` / `ShowWindow` / `UpdateWindow` 产生 `WM_CREATE` / `WM_SIZE` / `WM_PAINT`；`SendMessage` 同步、`PostMessage` 异步；`PeekMessage(PM_NOREMOVE)` 不移除；`GetMessage` 取走；`WM_QUIT`；`SetTimer` → `WM_TIMER`；`WaitMessage`；关闭确认（`MessageBoxA` 的 headless 默认） | pure | M2a |
| **B7** | **region**（pure）：矩形 / 椭圆 / 多边形；`CombineRgn` 的 AND/OR/DIFF/COPY；`GetRegionData` 往返；`PtInRegion` / `GetRgnBox`；`GetUpdateRgn` 与 `InvalidateRect` / `ValidateRect` | pure | M2b |
| **B8** | **M2 族无 stub**：`user32` / `gdi32` / `imm32` / `version` / `shell32` / `shlwapi` / `imagehlp` / `oleaut32` / `ole32` 的 stub = 0，除非键在 `SCOPE.md` 的偏差表（菜单 / IME / 注册表 / 摇杆 / 音频 / 视频）。★ 若 trace 证明某条只在 M4 的音频/视频路径上，**逐键**列进 M4 单或 SCOPE 例外；⛔ 不按模块整族放行、⛔ 不静默 | tool | M2b |
| **B9** | **交互 smoke**（assets，可选）：真实 SDL 窗口起来、标题可见、`WM_CLOSE` 走确认、退出不挂起 | assets | M2b |
| **B10** | **性能与工作量报告**：`--bench-frame N` 报指令数 / 帧时间 / MIPS；记录 DDraw+GDI 的实际投入与撞到的新语义数；**不做自动裁决**（由用户后续决定） | 报告 | M2a 出口 |

★ B4 的"没有执行到的未实现导入" = 现有 `tests/base_coverage.cpp` 的同一判据，但对象换成"到第一帧"的 trace。
★ **B2 / B4 的"第一帧"形状是 DDraw 味道的（`Flip >= 1`、trace 里出现 `Blt`/`BltFast`）**；而本安装
`set:CreateObject = 2` ⇒ 启动路径走 D3D9（§F2），第一条渲染指令是 `Clear`（`0x498B82`）而不是 `Flip`。
⇒ 这两条判据**要等路线裁决（§F2 的 A/B/C）之后再按裁决改写**；在那之前它们是"DDraw 后端"的判据，
不是本安装第一帧的判据。
★ B5 的"截获字符串"= 从 trace 里取 `TextOutA` / GDI 调用参数的副本，或者由夹具固定一段测试串；
  ⛔ 不要把"我看了眼截图"当判据。

## 6. 切片

> 量级是"一个人、熟悉本仓、不带外部等待"的人周；含不确定性，是量级不是承诺。
> 依赖：P0 → P1 → P2 → P5a；P3 可与 P1/P2 并行；P4 的小族部分可与 P1/P2 并行。
> ★ P0 里的"启动解锁"（图标 / 版本 / 假窗口）**必须先于 P5a**：版本检查在 `CreateWindowEx` 之前，
> 失败会直接退出（F6），不是"画面阶段"的问题。

| # | 切片 | 做什么 | 判据 | 量级 |
|---|---|---|---|---|
| **P0** | headless 窗口 + 完整消息泵 + 机制 | G1 导出转发表 · G2 COM 对象/vtable/refcount · G3 嵌套 guest 回调栈 · G4 guest surface arena · G5 `--appdata` + headless present + 帧哈希 + `--time-limit` · **headless 窗口**（句柄/类/客户区/位置）· **完整消息泵**（Get/Peek/Post/Send/Translate/Dispatch/Wait/Quit、`WM_PAINT` 合并、`WM_TIMER`、`PeekMessage(PM_NOREMOVE)`）· **启动解锁**：`LoadIconA` / `LoadCursorA` / `GetStockObject` / `RegisterClassExA` / `CreateWindowExA` / `ShowWindow` / `UpdateWindow` + `GetSystemMetrics` / `GetAsyncKeyState`（headless 固定值 / 无键）+ **AGE.EXE + 真实 AGERC 镜像**的 PE 资源 / `version` 3（D6；`--agerc` 可覆盖；缺镜像 ⇒ 126） | B1 + B6；pure 夹具；headless 下 `CreateWindowEx` → `WM_CREATE` → `WM_SIZE` → `WM_PAINT` 序列正确；boot 越过 `0x4BA9AA`（安装树根件即免壳件；⛔ 不回退 `DATA1.ALF` —— 拿不到根件就停在 0 + 126） | 4–5 周 |
| **P1** | DDraw 2D 核心 | DDraw/DirectDrawSurface 对象与 vtable（只填用到的 + 未用到的 loud trap）· `CreateSurface` / `Blt` / `BltFast` / `SetColorKey` / `Lock` / `Unlock` / `GetDC` / `ReleaseDC` / `Flip` / `Set/GetDisplayMode` / `SetCooperativeLevel` / `GetAvailableVidMem` / **`EnumDisplayModes` + guest 回调** · 颜色键 / 裁剪 / 16·32bpp · surface 在 guest 内存 | B2；到 DDraw init 不 trap；首个 surface 创建 + 首帧 present ★ **现实定位：已实现，但本安装的启动路径不经过它（见 §F2）；它是同一条线的另一条渲染后端，不是"没用"，只是不能当第一帧的前置** | 4–6 周 |
| **P2** | GDI 字体 / 文本 | GDI 对象表 / DC（DISPLAY + surface）· `CreateFontIndirectA` / `SelectObject` / `DeleteObject` / `GetCurrentObject` / `GetStockObject` · FreeType 后端 + Amayui CN · `GetTextMetricsA` / `EnumFontFamiliesExA` / `AddFontMemResourceEx` · `GetGlyphOutlineA` 1/17/65 · `GetTextExtentPoint32A` · `TextOutA` | B3；文本自洽；到第一段文本不 trap | 3–5 周 |
| **P3** | 真实 SDL 窗口 / 输入 | 只换**呈现与输入后端**：SDL 窗口 + 纹理上传 · SDL 键鼠 → 消息 + `GetAsyncKeyState` 影子 · 光标 / 焦点 / resize · 关闭确认（`MessageBoxA`：headless 一律 IDOK；交互只对 MB_OKCANCEL/YESNO 弹 SDL，其余记日志返回 IDOK）· **不改** P0 的窗口/消息语义 | B6 在 P3 之后仍绿；交互 smoke 能点、能关 | 2–3 周 |
| **P4** | 其余小族 / region | `SHGetSpecialFolderPathA` / `PathRemoveFileSpecA` / `MakeSureDirectoryPathExists` · COM/VARIANT 5 · `ImmAssociateContext` · GDI region + `GetUpdateRgn` / `ValidateRgn` · B8 的例外表（M4 路径逐键列出） | B7；B8 的族内 stub 归零；存档名/特殊目录不炸 | 2–3 周 |
| **P5a** | 第一帧集成 | 从 `_WinMain@16` 跑到 `Flip >= 1`（运行上限 **1 分钟墙钟**，`--time-limit 60`）；逐条 trace 修 gap；帧哈希 / 文本像素 / 性能数字 | B4 / B5 / B10 ★ **判据形状随路线裁决而变**：DDraw 的 `Flip` vs D3D9 的 `Clear`/`Present`（见 §6.1） | 2–3 周 |
| **P5b** | 标题 / 交互集成 | 跑到标题（若影片 skip 不阻塞）；点进 / 退出；关闭确认；M2b smoke | B9 | 1–2 周 |
| **P6** | 收口 | M2 族 stub 归零 / `lift check` / 变异条目 / 文档漂移 / 工作量报告 | B8 + 工作量报告 | 1 周 |

- **M2a = P0 + P1 + P2 + P5a ≈ 13–19 人周**（3.0–4.4 人月）。
- **M2b = P3 + P4 + P5b + P6 ≈ 6–9 人周**（1.4–2.1 人月）。
- **合计 ≈ 19–28 人周（4.4–6.5 人月）**；这是**评估数**，不是自动闸门（D8）。
- 一个人做：按 P0→P1→P2→P5a→P3/P4 并行→P5b→P6。
- 两个人做：P0 后，一人 P1+P2，一人 P3+P4；P5a 汇合。日历可压到 ~12–16 周，但**成本口径不变**。

### 6.1 P5a 的前置变化（2026-11-05 取证后）

★ 结论见 §F2：本安装 `set:CreateObject = 2` ⇒ **第一帧的真正前置从 DDraw 变成 D3D9 设备创建**。
下面这份集合是"到 **第一个 D3D9 设备创建成功**"的最小面（按 EA 顺序；⛔ 这不是新切片，是给 P5a / M3 的
**输入**，要不要按这条路走由 §F2 的裁决决定）：

| # | EA | 调什么 | 需要的形状（宿主侧） |
|---|---|---|---|
| 1 | `0x498D0F` | `d3d9!Direct3DCreate9(0x20)` | **唯一的 d3d9 导入**；返回 IDirect3D9（现在 boot 就停在这里） |
| 2 | `0x498D44` | IDirect3D9 **#8** `GetAdapterDisplayMode(0, &mode)` | `D3DDISPLAYMODE`（**16 B**：Width **+0** / Height **+4** / RefreshRate **+8** / Format **+0xC**；★ 2026-11-05 实测纠正，原写"12 B / Width·Height·Format"是错的：`0x498C71` 读 +0xC 当 `D3DFORMAT`） |
| 3 | `0x498ECA` | `sub_498BC0` →（条件）`sub_4983E0` | IDirect3D9 **#6** `GetAdapterModeCount` / **#7** `EnumAdapterModes`（只有目标模式被判无效时才走） |
| 4 | `0x497F47`（由 `0x49904B` / `0x499067` / `0x49908C` / `0x4990A2` 调入） | IDirect3D9 **#16** `CreateDevice(0, type, hwnd, flags, &pp, &out)` | `D3DPRESENT_PARAMETERS`（**56 B**，`0x498F0A memset 0x38`；`+0/+4/+8` = 宽/高/格式、`+28` = `hDeviceWindow`、`+36` `EnableAutoDepthStencil=1`、`+40` `AutoDepthStencilFormat=75`、`+44` `Flags=1`）；引擎会重试 DeviceType ∈ {HAL, REF} × BehaviorFlags ∈ {`0x44`, `0x84`, `0x24`} |
| 5 | `0x4990C8` | IDirect3DDevice9 **#20** `SetDialogBoxMode(TRUE)` | 错误串「ダイアログ描画モードの設定に失敗しました」 |
| 6 | `0x499126` | IDirect3DDevice9 **#7** `GetDeviceCaps(&caps)` | 只读 `MaxTextureWidth/Height` 并与 `set:TexWidth` / `set:TexHeight` 比 |
| 7 | `0x49918D` | IDirect3D9 **#10** `CheckDeviceFormat` | 查 A8R8G8B8 渲染目标格式（失败只记日志，不致命） |
| 8 | `0x498B82`（由 `0x4991BF` 调入 `sub_498B60`） | IDirect3DDevice9 **#43** `Clear(0, NULL, 3, 0, 1.0f, 0)` | **第一次渲染动作**（错误串「ClearTarget エラー」） |
| — | `0x498D05` / `0x498D7E` | `[old+8]` `Release`（旧 IDirect3D9 / 旧设备） | 只在重入（换分辨率 / Reset）时有对象；首次为空 |

- 之后 `sub_406730` 还在 `0x406B37` 调 `sub_4A6EE0`（把渲染器挂到设备上、建纹理/顶点管理）—— 属"设备已建好之后"。
- ⛔ **`Present`（IDirect3DDevice9 #17，vtable `+0x44`）不在这一段**：到第一次 Present 还差哪些
  （`BeginScene`/`EndScene`/纹理上传/精灵）**未测** —— boot 还没到。
- ⚠ 这套形状是 **M3 的 D3D9 簇**（`SCOPE.md` §1 现在写"3D 不做 / 走 stub"）；把它搬进 M2a 是**路线改动**，
  不是补一个 shim。⇒ 见需求单「M2 路线取证：显示初始化走 DDraw 还是 D3D9」。

#### 6.1.1 实测接着往下走的三段（2026-11-05；上表之后）

用户 2026-11-05 裁决"确认需要实现 D3D 接口"（`REQ-01M6KTYX5ZYENMPFHHNQTET254`，done）⇒ 上表 8 条已全部落地
（`REQ-01M6MH5M975Q63AH85F03RVQFD`，`src/runtime/d3d9.*` + `src/shims/d3d9.cpp` + `tests/d3d9.cpp`）。
之后 boot 又往前退了两站，各有单：

1. **`d3dx9_43!D3DXCreateSprite`**（`0x4A2BD0`，`sub_4A2BA0`）⇒ `ID3DXSprite`（Begin/Draw/End 真合成：调制色 × 纹理、
   包围盒与源矩形双向裁剪、`ALPHABLEND` 走 `src*a + dst*(1-a)`），顺带 `D3DXCreateTexture` + `IDirect3DTexture9` 的
   `GetLevelDesc`/`LockRect`（纹理像素在 `SurfaceArena`）+ 7 条 `D3DXMatrix*`。单：`REQ-01M6MJRC587XWXR325K4N2JXMF`。
2. **`IDirect3DDevice9` 的 fixed-function 流水线**：`SetTransform(+0xB0)` / `SetRenderState` / `SetTexture` /
   `SetSamplerState` / `SetFVF` / `BeginScene`+`EndScene` / `DrawPrimitive*` / `Present` —— **已落地**
   （`REQ-01M6MNR9M7S5M5711J79F9K32A`，`src/runtime/d3d9_pipeline.*` + `tests/d3d9_pipeline.cpp`）。
   ★ 这一段不是"3D 内容"，而是**任何一帧都要走的渲染管线**（引擎用 WORLD/VIEW/PROJECTION 三个矩阵摆 2D 精灵）。
   光栅口径：重心坐标 + 边界规则、视图体裁剪、最近点采样、`ALPHABLEND` 线性混合、Z 按状态开关 —— **自洽近似，⛔ 不冒充真机**。
3. **2026-11-05 已过：`Vfs::resolve` 的 guest 根前缀**（`REQ-01M6MPJ147RF41GJECZECXKC20`）。原来版本读取
   `version!GetFileVersionInfoSizeA("<guest根>\AGE.EXE")` 因为 guest 内存是 CP932、Vfs 内部是 UTF-8 而解析失败
   ⇒ `MessageBox` ⇒ 退出。现在 `resolve` 的匹配顺序是**四档**（overlay → guest 根前缀 → `--appdata` → 相对路径），
   前缀只认**完整段**（`…\天結X` / 根只出现在中间 ⇒ 不命中），路径日志统一走 `guestPathForLog()`（CP932 → UTF-8）。
4. **2026-11-05 已过：音频 silent 层**（`REQ-01M6MQPHSVZZSB8D4221Z0N1JW`）。`DSOUND.DLL` 用假模块 + `GetProcAddress`
   只认引擎真取的 `DirectSoundCreate`（`.lst` `0x4B5C9F` / `0x4B5CA6`；整份语料没有 `DirectSoundCreate8` / `EnumerateA`），
   `IDirectSound8` / `IDirectSoundBuffer8` 的静音实现**记账**（PCM 字节 176400、Play 次数、`WAVEFORMATEX` 原值），
   `Lock` 给真 guest 内存。顺带 `MessageBoxA` 现在把 caption/text 按 CP932 → UTF-8 打出来（定位 fatal 弹框不再靠反查 `.lst`）。
5. **★ 现卡点 = 线程（需裁决）**：`sub_4B5CF0` 尾部 `0x004B5FE4 call CreateThread`（DirectSound 的混音线程，
   线程体 `sub_4B5AA0`，随后 `ResumeThread` / `SetThreadPriority`，**无 NULL 检查、无分支**）⇒ boot 停在
   `unimplemented @ eip=7FFE01CC`，`insns = 109,536,868`，**0 条 MessageBoxA**。
   按 `SCOPE.md` §1 / `README.md` §2.5（guest 单线程，线程族 = 硬错误）这一族**不能顺手做** ⇒
   裁决单 `REQ-01M6MQQA737CB8BAENQK088GP4`（伪线程 vs 真线程两个方向 + 判据）。
6. 再往后若撞到图像文件解码（`D3DXCreateTextureFromFileInMemory` 一族）或 effect 编译 ⇒ 单独评估（⛔ 不猜像素）。

## 7. 落点与依赖

```text
apps/emulator-lift/
  src/runtime/
    guest_call.{h,cpp}        ← G3 嵌套 guest 回调栈（WndProc / SendMessage / MessageBox）
    com.{h,cpp}               ← G2 guest 对象 / vtable / refcount / 动态导出 thunk 分配
    surface.{h,cpp}           ← G4 guest surface arena（像素 + pitch + 格式 + 锁计数）
    pe_resource.{h,cpp}       ← PE .rsrc（RT_ICON / RT_GROUP_ICON / VS_VERSIONINFO）
    font.{h,cpp}              ← FreeType 桥（face / size / metrics / GGO 位图）
    region.{h,cpp}            ← GDI region（矩形表 + 扫描线填充 + combine）
    host_window.{h,cpp}       ← 假窗口 / 真窗口的公共形状（类表 / 客户区 / 消息队列）
    present.{h,cpp}           ← offscreen / SDL 两个 present 后端
  src/shims/
    user32_window.cpp         ← 窗口 / 类 / 位置 / 客户区 / 计时器
    user32_message.cpp        ← Get/Peek/Post/Send/Translate/Dispatch/Wait/Quit
    user32_input.cpp          ← 键鼠 / 光标 / GetAsyncKeyState / 焦点
    user32_region.cpp         ← GetUpdateRgn / ValidateRgn（与 gdi32_region 共用 region 类型）
    user32_resource.cpp       ← LoadIconA / LoadCursor* / MessageBoxA（关闭确认）
    gdi32_object.cpp          ← 对象表 / DC / SelectObject / DeleteObject / GetStockObject
    gdi32_font.cpp            ← CreateFontIndirectA / metrics / glyph / text / extent
    gdi32_region.cpp          ← Create*Rgn / CombineRgn / GetRegionData / PtInRegion
    ddraw.cpp                 ← DirectDraw / Surface 的 vtable 与实现
  tests/
    mechanism.cpp             ← B1：导出转发 / COM / 嵌套回调
    ddraw.cpp                 ← B2
    gdi.cpp                   ← B3
    window.cpp                ← B6
    region.cpp                ← B7
    first_frame.cpp           ← B4（assets）
    first_text.cpp            ← B5（assets）
```

- **依赖**：`freetype` 加进 `vcpkg.json`（`find_package(Freetype REQUIRED)`）；`sdl3` / `zydis` 已有。
- **CMake**：pure 测试顶层注册；`first_frame` / `first_text` 放在 `if(EXISTS "${AMAYUI_GAME_DIR}" OR EXISTS "${AGE_INSTALL}")` 里，
  `SKIP_RETURN_CODE 77`；缺资产时**不注册**，⛔ 不"跑绿了"。
- **测试根**：`--game-dir` 指向安装树，`--appdata` 指向临时目录（D5）；测试的存档/配置写到 `--appdata`。
  引擎若仍写到 `--game-dir` 的相对路径（`SYS4REG.INI` / "异常终止"标记），测试必须**备份并恢复**，
  并在结束时断言安装树未被改动；⛔ 不靠"下次跑之前运气好"。
- ★ 所有新 stub 必须走 `addUnimplemented` + reason；实现后从 `register.cpp` 的未实现清单里删掉，
  `pnpm tools lift sync --write` 更新 `data/lift/overrides.json`，`pnpm tools lift check` 必须绿。

## 8. 测试与守卫

1. **pure 夹具自己也得会红**：
   - DDraw：颜色键改成"永远复制" ⇒ 颜色键用例红；裁剪切掉 ⇒ 越界用例红；pitch 改成 width ⇒ Lock 用例红。
   - GDI：advance 用 `floor` 改成 `round` ⇒ 自洽用例红；GRAY4 用 16 级 ⇒ 量化用例红。
   - 回调：嵌套回调弹参少 4 字节 ⇒ 栈平衡用例红。
   - 窗口：`PeekMessage(PM_NOREMOVE)` 实现成移除 ⇒ 消息序列用例红。
   - 把这些加进 `tools/mutate-check.mjs` 的清单（C++ 侧也可照 M1 的做法手动施加 + 确认红）。
2. **assets 夹具**：B4/B5 必须断言"**真的到了**"（`Flip >= 1`、`GetGlyphOutline > 0`），
   运行上限 **1 分钟墙钟**（`--time-limit 60`）；⛔ 不要"trace 为空 ⇒ 动态缺口 0 ⇒ 假绿"（M1 已经踩过这条）。
3. **确定性**：虚拟时钟（`--real-time` 才接宿主钟，且 B4/B5 自动跳过并明说）；
   `cpuid` / `rdtsc` 固定值；`FindFirstFile` 排序；宿主随机源为零；
   `--trace-shims` 两次运行逐字节相同；帧哈希两次相同（**同平台同构建**，不跨平台比字节）。
   ★ 1 分钟是**墙钟上限**，不是确定性判据：确定性比对用固定输入 + 固定 `--max-insns`/`--max-frames`
   （或同一 `--time-limit` 下比较"最后一帧"时，必须在 trace 里记录帧号并接受"可能少一帧"）。
4. **`--appdata` / `--game-dir` 卫生**：测试不得写 `--game-dir`；用 `git status` / 文件 mtime 证明安装树未变
   （M1 的纪律延续）；`--appdata` 用独立临时目录，跑完即弃。
5. **A5 扩展**：`base_coverage` 现在只看 L2；M2 要看"到第一帧"的 trace。M2 族内 stub 的归零用
   `pnpm tools lift check` + 一条新的 `lift-m2-coverage`（B8），⛔ 不靠人肉看报告。
6. **性能**：`--bench-frame N` 输出 `insns/frame` / `ms/frame` / `MIPS`；**不设硬门槛**（M1 的 bench 也是只报数），
   在**工作量报告**里给结论（能不能 10fps+）。

## 9. 风险

| # | 风险 | 早期信号 | 处置 |
|---|---|---|---|
| R1 | **性能**：解释器 13 MIPS，一帧可能远超 21 万条 | `--bench-frame` 的 ms/frame > 100 | 先测；不够就单开"线程化解释器 / 热块"单，⛔ 不混进 M2 功能判定；若 10fps 都到不了，把性能单提到 M2 之外单独立项 |
| R2 | **影片 / LOGO 依赖 M4**：MF 全 `E_FAIL`，影片 opcode 可能不推进 | 第一帧前卡在影片状态 / `MF*` 调用 | 先用"skip 影片 + 强制标题"的 fixture 验证 2D；若成功路径必须影片推进，开 M4 单，⛔ M2 不自作主张实现 MF |
| R3 | **版本 / AGERC 检查**：`set:RCVersion` 与 AGERC 版本不符会 `MessageBox` 后退出 | boot 到 `0x4BAF0A` 附近退出 | 实现 PE `VS_VERSIONINFO`（D6）；AGERC 的版本从**装载的镜像**读（`39.1.0.0` → `atoi` = 39，见 [`agerc-image.md`](agerc-image.md)）；不 fake 成"相等"除非明确登记偏差 |
| R4 | **C++ 异常**：主循环退出走 `_CxxThrowException`；SCOPE 是硬错误 | `__CxxThrowException` on success path | 测试用 `--max-frames` 停，不走 `WM_QUIT`；审计 throw 点；若成功路径也 throw，单独开单（可能要 SEH/C++ unwind，属大工程） |
| R5 | **线程**：服务线程 / 第二消息线程；SCOPE 是硬 trap | 启动路径出现 `CreateThread` / `_beginthreadex` | 按 SCOPE 记录 + 开单；⛔ 不在 M2 里开并发（要先有证据、改 §2.5） |
| R6 | **x87 / SSE 未实现**：渲染/文本路径撞到 `unimplemented` | `--trace-shims` 前最后一个 EA 在 `exec_fpu` | 先补指令（或确认 M1 的 S4 复核单）；⛔ 不要把它当"DDraw 画错了" |
| R7 | **DDraw 标志 / 格式超集**：引擎用了未实现的 flag/rop/stretch | 首次 `Blt` 的参数与预期不符 | 未实现 flag ⇒ **loud trap** 列进清单；按 trace 顺序补，⛔ 不静默忽略 |
| R8 | **字体度量偏差**：宿主字体 ascent 不同 ⇒ 行高 / 基线偏 | 文本整体上下偏 1–3px / 行距不对 | `--baseline-lift`；A/B 对照引擎字面名；按"自洽"判据先过关 |
| R9 | **安装树被写 / 异常终止标记** | 下一次 boot 直接退出 / `git status` 脏 | `--appdata`（D5）承接 AppData；测试前备份/恢复 `--game-dir` 里被写的文件；失败时保留 `--appdata` 供复现 |
| R10 | **静态计数是下界**：LoadIconA/ShowCursor 0 站点但真调用 | trace 里出现 census 没记的调用 | 以 trace 为准；工作量按"撞到的"算，⛔ 不按静态站点数排 |
| R11 | **消息重入**：WndProc 里 `SendMessage` / `DestroyWindow` 造成递归 | 栈不平衡 / 死循环 | G3 用 LIFO 栈 + 深度上限 + 诊断；窗口夹具覆盖嵌套 `SendMessage` |
| R12 | **估时偏差**：M2a / DDraw+GDI 超出本文件的区间 | P5a 结束的实际投入 | 记录实际人周与新语义数，写进工作量报告；⛔ 不自动停手、不自动改路线（由用户决定） |
| R13 | **路线错配**：M2a 的"第一帧"按 DDraw 排（P1 + B2/B4 的 `Flip`/`Blt` 判据），而本安装的启动路径走 D3D9（`set:CreateObject = 2`，§F2）⇒ 即使 DDraw/GDI 全做完，boot 仍停在 `d3d9!Direct3DCreate9` | boot 的 `stop` 一直是 `d3d9!Direct3DCreate9`（`eip=7FFE00AC`），`--trace-shims` 无任何 `DDRAW.DLL` 装载 | 先把路线裁决成一张单（M2 下的"路线取证"），按裁决**改 P5a 前置与 B4 判据**；⛔ 不要靠"再补几个 DDraw API"来推进 boot；也⛔ 不要把 §F2 的 D3D9 形状当成"补一条 shim"（那是 M3 的簇） |

## 10. M2 明确不做

- ⛔ 菜单栏交互（`SCOPE.md` §1 永久 no-op）；⛔ 注册表持久化；⛔ 摇杆；⛔ 音频 / 视频（M4）；
  ⛔ 3D / Live2D（M3）。
- ⛔ 完整 IME（`ImmAssociateContext` 返回 NULL 即可；存档命名输入不在 M2 判据里）。
- ⛔ 触摸 / 手势（动态导出返回 `NULL` = "这个 OS 没有"，不是影子实现）。
- ⛔ 多显示器 / DPI 缩放 / 真独占全屏（windowed / borderless 映射；`SetCooperativeLevel` 接受但不独占）。
- ⛔ 完整 SEH / C++ 展开；⛔ 线程；⛔ 任意码页（沿用 M1 的 CP932 + ASCII；其它码页仍只记日志）。
- ⛔ JIT / 动态翻译（性能问题单开）；⛔ 与真机逐像素一致（只做自洽 + 确定性 + 已知不变量）。
- ⛔ 用 `apps/emulator-cxx` 的 core 当 shim；⛔ 嵌 Wine 系组件（README §1.1）。

## 11. 对上游结论的影响（要回写）

1. **工作量数字**：P5a 结束的**工作量报告**记录"DDraw + GDI 实测人周 / 撞到的新语义数 / 帧时间"，
   供用户后续决策；⛔ 不直接改 `platform-effort.md` 的结论，也不自动触发"停手"。
2. **README / docs 索引**：M2 计划挂到 `docs/README.md` 与 `../README.md` §5 的"实现计划"指针。
3. **需求树**：M2 开工时把 P0–P6 登记成 M2 下的子节点（`REQ-01M6H6G6DSRG1ZRD810Z0HTH8V`），
   M2a/M2b 用状态区分；⛔ 不在本文件里手写进度。
4. **AGENTS.md §11** 目前写着"唯一在维护的模拟器是 `apps/emulator-cxx`"——本线是第二条线（`docs/03-emulator/README.md` §7），
   但 M2 的结论可能影响"两条线"的措辞；要改走用户裁决，⛔ 不顺手改。

## 12. 已定 / 剩余（2026-11-04 用户口径）

**已定（按这些写实现与估算）**：

1. **D1/D2**：先 **headless 窗口**；但**消息泵系统完整**（不是最小假件）。真实 SDL 窗口/输入后做。
2. **D3**：字体照 `apps/emulator-cxx` 的已有策略：宿主 **Amayui CN** + 基线 **+2px** + 粗体单趟/异色两趟；
   登记为 `SCOPE.md` 的平台偏差，⛔ 不写进 spec。
3. **D5**：新增 `--appdata <host-path>`；特殊目录（AppData / LocalAppData / Personal）映射到该路径；
   ⛔ 不引入通用 `--work-dir` 覆盖。
4. **D6**：直接读 **AGE.EXE** 的 PE 资源；**AGERC 也按真实模型**：PE 节映射 + 真实 ImageBase / HINSTANCE +
   资源/版本从镜像解析。镜像来源**只有两段**：`--agerc`（显式覆盖）→ `--game-dir/AGERC.DLL`；
   **拿不到 ⇒ 0 + 126**（⛔ 不回退 `DATA1.ALF`：引擎本身也不回退）。**免壳件本机就有**
   （安装树根件 `6241de66…`，是 ALF 条目 `f48d1c0f…` 的汉化派生；ALF 条目只作取证对照、不是装载来源）
   ⇒ ⛔ 不需要另外的拆壳件、⛔ 伪 HICON 只作应急。
   三个导出仍按 `SCOPE.md` §1 的模态 no-op stub；4 条无人调用的 `FindResource*` 注册删掉。
   取证见 [`agerc-image.md`](agerc-image.md)。
5. **D8**：**不设自动停手规则**。本文件只给工作量评估（19–28 人周，M2a 13–19 / M2b 6–9）；
   后续是否做、做多少由用户决定。
6. **探针上限**：第一帧/文本的探索运行用 **1 分钟墙钟上限**（`--time-limit 60`）；到点即停并报告
   当前 EA / 帧数。确定性比对另用固定输入 + 固定 `--max-insns`/`--max-frames`。

**剩余（需要时再问用户）**：

- ~~本机没有 AGERC 拆壳件~~（**已否证**，见 [`agerc-image.md`](agerc-image.md)）：免壳件就在 `DATA1.ALF`
  第 21072 条，安装树根件是它的汉化派生（图标/版本逐字节相同）。★ 这条**只是取证对照** —— 装载来源只有
  `--game-dir/AGERC.DLL`（或 `--agerc`），⛔ 不为 ALF 条目写读取分支（用户 2026-11-05 裁决）。
  本机**可以**验图标解析与版本读取；
  仍**未测**的是运行期闭合（正版机上把根目录 `AGERC.DLL` 改名看是否报「AGERC.DLLを読み込み出来ません」）
  与 AsProtect 运行期解包后的内存镜像。
- M2a 跑完第一帧后：是否继续 M2b（真实窗口/输入/标题）—— 由用户按工作量报告决定。
- 是否把 P0–P6 登记成需求树子节点 / 把 M2 置 `doing` —— 由用户拍板；本文件不手写进度。
