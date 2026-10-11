# 只读取证 · 引擎启动时按什么条件把哪个 global 置成什么（**不落地**；落地是父单的事）

- id: REQ-01M4H0R2A2Z9H9JF9SDW26WYM3
- type: req
- status: done
- parent: REQ-01M4B416EKPVZWHFWT283C1T3Q
- done_reason: 取证完成（负结果）：『按配置存在性置 global』这个动作不存在（至少这条链上）—— ① SYSTEM4 的 jcc 值来自紧邻它前一条 load-int（sub_42DF40 的 0x42DFA2-0x42DFA7）的**无条件**写回；② SYS4REG.INI/SYS3REG.INI 探针 sub_4900F0 只返回 BOOL（0x49035C cmp eax,-1 ⇒ 建目录返 0 / CloseHandle 返 1），体内不碰 Engine+0x5D800；③ 键的取值原语 sub_418A30 三条臂返回的都是表下标，两个调用点都 push 1（操作数序号）。★ 边界：没有穷尽搜索别处是否写 global:int[5]、也没定位 Engine+0x5D800 的分配点 ⇒ 不能说『引擎永不置位』。产物：台账 KN-01M4N26E7E7S7V3X1P7N0H4C5T + KN-01M4N24BGH3G3J423R634V0S44；本节点只取证、未改任何代码（判据已遵守）。
- tags: [engine, ledger]

## 取证结论（**负结果**，但正是取证要的东西）

**问题原文**：引擎启动时按什么条件把哪个 global（或帧字段）置成什么值。

**结论**：**按"配置存在性"置 global 这个动作不存在**（至少在这条链上不成立）。三条逐字证据（锚 = EA，全部已进台账）：

| # | 结论 | 逐字判据（EA） |
|---|---|---|
| 1 | `SYSTEM4` 那条 `jcc` 的取值**来自紧邻它前一条 `load-int` 自己的写回**，不是引擎初值 | 脚本视图 `dist/views/data/SYSTEM4.BIN.txt` 第 71-72 行**连续两条**：`load-int (global-int 5)` → `jcc (global-int 5) ffffffff label_0000068c`；`load-int` 的写回**无条件**：`sub_42DF40` 的 `0x42DF98 test eax,eax` / `0x42DF9A jz`（未命中 ⇒ 0）/ `0x42DFA2 push eax` / `0x42DFA3 push 1` / `0x42DFA7 call sub_42B4B0` |
| 2 | "配置存在性"探针**只返回 BOOL**，体内**不碰** `Engine+0x5D800` | `sub_4900F0`：`0x490333 push offset aSys4regIni`（退路 `0x49033A` `"SYS3REG.INI"`）→ `0x490341 call sub_407FD0` → `0x490356 call CreateFileA`（`dwCreationDisposition=3`=OPEN_EXISTING / `dwFlagsAndAttributes=0x80`）→ `0x49035C cmp eax,0FFFFFFFFh`：打不开 ⇒ `0x4903A3 MakeSureDirectoryPathExists` + `retn 8`（**0**）；打得开 ⇒ `CloseHandle` + `retn 8`（**1**）。★ 全语料 `5D800h` **39 处，没有一处**落在它体内 |
| 3 | 原前提"键取自该变量当时的值 ⇒ 必然由引擎侧设置"**不成立** | 键的取值原语 `sub_418A30`（`0x418A30`）三条臂（`0x418A5B` type 3 / `0x418A60` type 6 / `0x418A65` type 12）**返回的都是表下标**；两个调用点（`0x42DF6F` / `0x434F8F`）都 `push 1`（**操作数序号**）⇒ 键 = `"\x03 00000001"`，**与操作数当时的值无关** |

**另外两条与被证伪前提直接相关的实测**
* `sub_4900F0` 的调用点只有两处（`0x490EC7` / `0x49309A`），而 `sub_490590` / `sub_492CB0` 在全语料里**没有任何 `call|jmp` 指向它们**（只能经 vtable 到达）⇒ 不能声称它在启动路径上。
* 全语料 `5D800h` 共 39 处，其中**只有 1 处是写**（`0x41AB03 mov [esi+5D800h],ecx`），其余 38 处全是读。

## ★★ 边界（**必须照写，不许扩**）

本节点**没有**穷尽搜索"别处是否写 `global:int[5]`"，也**没有**定位 `Engine+0x5D800` 的分配 / 初始化点 ⇒ 只能说到这里：
1. `SYS4REG.INI` / `SYS3REG.INI` 探针**不是**置 global 的动作；
2. `jcc` 的值来自它**前一条** `load-int` 的无条件写回；
3. `load-int` 的键 = **操作数序号**派生的下标。
⛔ **不能**由此说"引擎永不置位 `global5`"或"引擎侧没有配置初始化" —— 那两句本节点**没有**取证。

## 产物

* 台账：`config/global5-jcc-source-and-existence-probe`（`KN-01M4N26E7E7S7V3X1P7N0H4C5T`）· `Engine+0x5D880/operand-ordinal-to-table-index`（`KN-01M4N24BGH3G3J423R634V0S44`）。
* 本节点**只做取证、不改任何代码 / 脚本**（判据原文如此）—— 已遵守。
* 由本结论推出的**真实缺口**（键的口径）已另开单：`REQ-01M4N29QFPHW3X0VKP1YWGDDC7`（bug，挂在 `REQ-01M4B248NB8FWDN61E2NMK8N6V` 下）。
