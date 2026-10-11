# 能力缺口 · 启动时按配置存在性设置 global（⇒ 否则 LOADCONFIG 那条支永远走不到）

- id: REQ-01M4B416EKPVZWHFWT283C1T3Q
- type: req
- status: dropped
- parent: REQ-01M4B248NB8FWDN61E2NMK8N6V
- dropped_reason: 前提被证伪：原文那条因果链（load-int 的键取自该变量当时的值 ⇒ jcc 在任何 load-int 之前 ⇒ global5 那一刻必然是引擎初值 ⇒ 启动时由引擎侧按配置存在性置 global）三处都不成立 —— ① SYSTEM4.BIN.txt 第 71-72 行 load-int(global-int 5) 与 jcc(global-int 5) 是**连续两条**；② 键的取值原语 sub_418A30(0x418A30) 三条臂（0x418A5B type3 / 0x418A60 type6 / 0x418A65 type12）返回的都是**表下标**，两个调用点 0x42DF6F / 0x434F8F 都 push 1（操作数序号）⇒ 键与操作数当时的值无关；③ 配置存在性探针 sub_4900F0 只返回 BOOL（0x49035C cmp eax,-1），体内不碰 Engine+0x5D800，两个调用点所在的 sub_490590/sub_492CB0 全语料无 call/jmp 指向 ⇒ 既非置位动作也不在启动路径上。★ 边界：没有穷尽搜索别处是否写 global:int[5]、也没定位 Engine+0x5D800 的分配点 ⇒ 不能推出『引擎永不置位』。★ 真实缺口（键的口径）已另开单 REQ-01M4N29QFPHW3X0VKP1YWGDDC7（改它会推进启动链 ⇒ 本轮只开单）。

## ★★ 2026-10-11 处置：**前提被证伪 ⇒ 本单 `dropped`**（不是"做完了"，是"这件事的前提不成立"）

### 被证伪的是什么（逐字，锚 = EA）

原文的因果链是：
> `load-int` 的**键取自该变量当时的值**（初值 0 ⇒ 键 `\x03 00000000`）；`#56` 的 `jcc` 在**任何** `load-int` **之前**执行 ⇒ `global5` 那一刻必然是初值 ⇒ **启动时那个 global 由引擎侧设置**。

三处都不成立：

1. **"`jcc` 在任何 `load-int` 之前"** —— 脚本视图 `dist/views/data/SYSTEM4.BIN.txt` 第 71-72 行是**连续两条**：`load-int (global-int 5)` → `jcc (global-int 5) ffffffff label_0000068c`。原文把**指令序号**与**脚本行号**混了。
2. **"键取自该变量当时的值"** —— 那是**本层模拟器的近似**，不是引擎口径。引擎两处调用点（`0x42DF6F` / `0x434F8F`）都 `push 1`（**操作数序号**）+ `call sub_418A30`，而 `sub_418A30`（`0x418A30`）三条臂（`0x418A5B` type 3 / `0x418A60` type 6 / `0x418A65` type 12）**返回的都是表下标** ⇒ 键 = `"\x03 00000001"`。
3. **"引擎侧按配置存在性置 global"** —— 语料里那个"配置存在性"探针 `sub_4900F0`（`0x490333 "SYS4REG.INI"` / 退路 `0x49033A "SYS3REG.INI"` → `0x490356 CreateFileA` → `0x49035C cmp eax,-1`）**只返回 BOOL**（打不开 ⇒ `MakeSureDirectoryPathExists` + 返回 0；打得开 ⇒ `CloseHandle` + 返回 1），**体内不碰** `Engine+0x5D800`；它的两个调用点（`0x490EC7` / `0x49309A`）所在的 `sub_490590` / `sub_492CB0` 在全语料里**没有任何 `call|jmp` 指向它们**（只能经 vtable 到达）⇒ 既不是置位动作，也不能声称在启动路径上。

⇒ 这条分叉上**没有**"引擎侧按配置存在性置 global"这一步可落地；`jcc` 的值就是它前一条 `load-int` 的返回值。

### ★★ 边界（照写，不许扩）

本次取证**没有**穷尽搜索"别处是否写 `global:int[5]`"，也**没有**定位 `Engine+0x5D800` 的分配 / 初始化点（全语料 `5D800h` 39 处里**只有 1 处是写**：`0x41AB03 mov [esi+5D800h],ecx`；其余 38 处全是读）。
⇒ ⛔ **不能**由本单推出"引擎永不置位 `global5`"或"引擎侧没有配置初始化"。本单丢掉的是**原文那条因果链**，不是"引擎侧不可能有配置初始化"这个更弱的说法。

### 原"缺口"去哪了

原文的收口判据是"启动链真的走到 `LOADCONFIG`"。按新证据，这条支**不靠"引擎侧置位"**；真正挡在它前面的是**键的口径**（引擎用序号 / 表下标，本层用值）。
⇒ 已另开单：**`REQ-01M4N29QFPHW3X0VKP1YWGDDC7`**（bug，S2，挂在 `REQ-01M4B248NB8FWDN61E2NMK8N6V` 下）—— 它带可重跑的 `repro`（`tools/test/emulator-engine-scalars.test.mjs` 那条守卫 + 变异清单里"值→序号"的钉子）。
★ 改键口径**会推进启动链**（`load-int (global-int 5)` 紧挨 `jcc (global-int 5)`）⇒ 按用户限制**不在本轮落地**，这也是它只开单、不动手的原因。

## 已裁决口径（2026-10-10，保留作沿革）

本单的"落地"属**推进指令流程** ⇒ 按用户限制**不落地**，只到取证线；拆出只读取证子节点 `REQ-01M4H0R2A2Z9H9JF9SDW26WYM3`。**该子节点已于 2026-10-11 以负结果收口**（见其 `done_reason` 与 `data/ledger/` 两条观察）。

## 在此之前怎么量（保留，作为"绕过引擎侧初始化"的诚实口径）

以子脚本为根直接跑（`--root-id <id>`），并把结果钉进守卫
（`tools/test/emulator-headless-logo.assets.test.mjs` 的"三条子脚本各自跑到下一个明确缺口"）：

| 子脚本 | id | 结果 |
|---|---|---|
| `LOADCONFIG.BIN` | 21080 | 32 步 ⇒ 停在 `#33 0x61 lookup-array` |
| `CHECKCONFIG.BIN` | 20955 | 0 步 ⇒ 停在 `#0 0x2de` |
| `INITCONFIG.BIN` | 20956 | 跑通（含 `INITCONFIG0..4`）⇒ 停在 `INITCONFIG4#7 0x61 lookup-array` |

★ 这是**诚实**的：它量的是"这份脚本自己能跑多远"，并**显式注明**绕过了引擎侧的初始化；⛔ 不许把它说成"启动链走到了 LOADCONFIG"。
