# M2 路线取证：显示初始化走 DDraw 还是 D3D9（set:CreateObject 实测）

- id: REQ-01M6KTYX5ZYENMPFHHNQTET254
- type: req
- status: open
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- tags: [emulator-lift, m2, route]

## 一句话
本安装的**显示初始化走 D3D9**，不走 DDraw：`set:CreateObject` 实测 = **2**（bit0=0 / bit1=1）、
`set:DrawMode` = 1 ⇒ `sub_406730` 的 `& 2` 块生效、`& 1`（DDraw）块不生效；boot 停在
`d3d9!Direct3DCreate9`（`eip=7FFE00AC`）。⇒ M2 第一帧的真正前置是 **D3D9 设备创建**（原属 M3 的簇）。
取证细节与重跑命令：`repo:apps/emulator-lift/docs/plan-m2-2d-text.md` §F2 / §F2.2 / §6.1。

## 实测值（两条独立证据）
- 真源 `links/install/SYS4INI.BIN`（446,558 B，sha256 `b6738009…5c9ee1f`）= S4IC：`[300,312)` 三个 uint32
  `1692004 / 1692004 / 446246` + LZSS（`sub_436A80`：N=4096/F=18/阈值2/r0=N-F）⇒ 1,692,004 B
  = 21,109 条 ALF TOC + 尾部初始化块（解压偏移 1,691,024：`953`、`51`=对数；`CREATEOBJECT` 键在
  **1,691,032**，值 `"2"`；`DRAWMODE` = `"1"`）。读法链：`0x4BAB1A push "SYS4INI.BIN"` →
  `0x4BAB26 sub_414AC0`（`CreateFileA` 返回地址 `0x414B1F`）→ `sub_494220`（vtable 分派）→
  `0x494F92 __stricmp("CREATEOBJECT")` → `0x494F9F _atoi` → `0x494FBC Reg::set("set:CreateObject")`。
- 运行期（`--until ea`，只读运行期，不看代码）：`0x4068C7`（bit1 块首条）**停住**（insns 102,914,704）；
  `0x406759`（bit0 块首条）**停不到**，一路撞 `unimplemented @ 0x7FFE00AC`（insns 102,919,944）；
  `0x494F92` **停住**（insns 35,853,538）⇒ ini 键确实在启动路径上被解析。
  `--trace-shims` 全程无 `LoadLibraryA("DDRAW.DLL")` / 无 `DirectDrawCreate`。

## 分支证据（EA）
- bit0（DDraw）：`0x406751 test al,1` / `0x406753 jz loc_4068A9`；块内 `sub_43AE30`(0x40675F) →
  `sub_440A20`(0x406805) → `sub_450B50`(0x40685E)；失败各弹错误串并 `return 0`。
- bit1（D3D9）：`0x4068BF test al,2` / `0x4068C1 jz loc_406C3F`；块内 `sub_498CC0`(0x406ADA) →
  `sub_4A6EE0`(0x406B37)；失败弹「DirectX…」并 `return 0`。
- **顺序 if、不互斥**：bit0 块无收尾跳转，落到 `loc_4068A9`（bit1 判定）⇒ 值 3 时两段都跑；
  `& 1` 在 bit1 块内再读一次（`0x406BE3`）。
- **只有两位（就 `sub_406730` 与引擎自己的写值而言）**：全 `.lst` 共 **32** 处
  `push offset "set:CreateObject"`（1 处写入 `0x494FAE`，其余 31 处读取）；31 处读取的用法只有
  `test al,1` / `test al,2` / `and eax,(1<<DrawMode)` 后 `cmp` / 当 sprintf 参数 —— **没有 `& 4` / `& 8`**。
- `set:DrawMode` 选当前位：`0x4178F9` 算 `1 << DrawMode`，缺位就 `0x417908 push 3` 把 CreateObject
  **强改成 3**（→ DDraw 与 D3D9 都初始化）；本安装 `1<<1 = 2` 已满足 ⇒ 不触发。

## DDraw 是否 dead
**本安装的启动路径上：dead。** `sub_440A20`（完整 DDraw 初始化）唯一调用点 `0x406805`（bit0 块）；
`sub_43AE30` 两个调用点 = `0x40675F`（bit0 块）+ `0x406502`（`sub_4764B0` 的 chunk，`0x4764B6 jmp 0x4064E0`）。
⚠ 但"二进制里彻底不可达"**未证**：`sub_4764B0` 全 `.lst` 无直接 `call`（`disasm-fn callers` = 0 + 1 处取址，
表 `0x526B70` 首项；`0x4B5338` 有 `"AGE:IAGEService"` 服务名）⇒ 动态分派入口，是否会被脚本/服务调到 **未测**。
⛔ 不要据此删掉 P1 / `src/shims/ddraw.cpp`。

## D3D9 到"设备创建成功"的最小导出集合（EA 序）
- `0x498D0F` `d3d9!Direct3DCreate9(0x20)` —— **唯一的 d3d9 导入**
- `0x498D44` IDirect3D9 **#8 GetAdapterDisplayMode**(0, &D3DDISPLAYMODE)
- `0x498ECA`（条件）IDirect3D9 **#6 GetAdapterModeCount** / **#7 EnumAdapterModes**（`sub_4983E0`）
- `0x497F47` IDirect3D9 **#16 CreateDevice**（`D3DPRESENT_PARAMETERS` 56 B；重试 DeviceType 1/2 ×
  BehaviorFlags 0x44/0x84/0x24）
- `0x4990C8` IDirect3DDevice9 **#20 SetDialogBoxMode** · `0x499126` **#7 GetDeviceCaps**（D3DCAPS9 MaxTextureW/H）
- `0x49918D` IDirect3D9 **#10 CheckDeviceFormat**
- `0x498B82` IDirect3DDevice9 **#43 Clear**(0,NULL,3,0,1.0f,0) —— **第一次渲染动作**
- ⛔ Present（#17）**不在**这一段；到第一次 Present 还差什么 = **未测**（boot 还没到）。

## 需要用户裁决（只列，不选）
- **A · 继续把 DDraw 当 M2a 的目标**：前提 = C 的全部前提（本安装不改配置就只有死路）；代价 = C 的全部风险；
  收益 = P1 已交付的 8 组夹具变成"启动路径上的代码"。
- **B · 承认走 D3D9，M2a 的"第一帧"改成"D3D9 设备创建 + 一次 Clear/Present"**：代价 = 把 **M3 的 D3D9 簇提前**
  （`SCOPE.md` §1 目前是"3D 不做 / 走 stub"）+ M2 判据 B2/B4 要按 D3D9 形状**改写**；收益 = 与实测一致、可推进。
- **C · 配置层让引擎走 DDraw**：前提 = ①**要同时改两个键**（`CREATEOBJECT=1` **且** `DRAWMODE=0`；只改一个会被
  `0x417908` 补成 3 ⇒ 两条都初始化）；②要**自己写 LZSS 压缩器**重打包 S4IC 载荷并改 `[300,312)` 三个长度
  （TOC 必须逐字节不变）；③⛔ 不改安装树 ⇒ 只能让 VFS 提供一份改过的副本（overlay）。
  风险 = 重压缩必须被引擎解压器接受 / DDraw-only 之后 3D 与 Live2D 是否仍依赖 D3D9 设备 **未测** /
  这算**平台偏差**不能进 spec / 测试输入多一档哈希（确定性比对变复杂）。
