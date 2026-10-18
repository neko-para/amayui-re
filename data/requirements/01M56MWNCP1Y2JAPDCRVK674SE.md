# 批 S2p · 未标最大的 5 个函数（载体 5 条）+ T_stdstring（std::string 布局适配器）+ 2 处类型缺口

- id: REQ-01M56MWNCP1Y2JAPDCRVK674SE
- type: req
- status: done
- parent: REQ-01M54KEF7X560CMPMGABG2W3CW
- done_reason: 数据类合并：5 条载体（全 strong，四判据交叉）+ std::string 适配器 + 2 处类型缺口；由 claims/propagate/calls 门禁与逐载体实测 after 背书（并记录"门禁全绿不给判类背书"）
- tags: [engine shape]

# S2p · 未标里最大的 5 个函数（载体 5 条）+ `T_stdstring`（`std::string` 布局适配器）+ 2 处类型缺口

按"未标函数里站点最多"排序取的 5 个（`sub_445D70` 233 · `sub_45FF00` 227 · `sub_410160` 197 · `sub_462040` 189 · `sub_4734F0` 178），
全部定类为 **strong** 并落地载体。

## 载体

| 载体 | 函数 | 类型 | 判据 | 合并后（解释/残差） |
|---|---|---|---|---|
| c111 | `sub_445D70` | `T_Sys3Draw*` | ②偏移 204/205（17 个偏移全表**只有** `T_Sys3Draw` 声明；次优 `T_DrawEffect` 51）+ ③3 个调用点全是 `lea ecx,[esi+1EE8h]` | **205 / 0** |
| c112 | `sub_45FF00` | `T_Font*` | ②135/170（24 个偏移排他，次优 `T_Sound` 39）+ ③`0x46947b mov ecx,ebx`，ebx = 调用方 `sub_469260`（已有载体 c107=`T_Font*`）的 this | 135 / 35 |
| c113 | `sub_410160` | `T_Command*` | ②130/179 + ③4 个调用点全传 `this`（k=0） | 130 / 53 |
| c114 | `sub_462040` | `T_Font*` | ②140/165 + ③1 个调用点传 `this` | 140 / 25 |
| c115 | `sub_4734F0` | `T_Font*` | ②77/153 + ③3 个调用点全传 `this` | 77 → **95** / 76 → **58**（见下） |

`c111` 的 ③ 用了一条很干净的链：Command 构造器在同一地址 `Command+0x1EE8` 调 `Sys3Draw` 构造器
（`0x41568c lea ecx,[esi+1EE8h]` → `0x415692 call sub_4B7D60` → `0x4b7d93` 写 `??_7Sys3Draw@@6B@`），
而 `types.json` 里 `T_Command+0x1ee8 : T_Sys3Draw` 早就是登记事实。

## 我按证据补的两处类型缺口

1. **`T_Sys3Draw+0x2044`**（`0x445e9e mov [esi+2044h], eax` ⇒ dword）—— 补上后 `c111` 从 204/1 → **205/0**。
2. **`T_Font+0x1cf28`**（`0x4652d9 mov [esi+1CF28h], ebx` ⇒ dword；该偏移有 **18 站点**）—— 补上后 `c115` 从 77/76 → **95/58**（18 条残差一次消掉）。

## 新类型 `T_stdstring`（`std::string` 布局适配器）——顺带解掉一处新冲突

`sub_410160`/`sub_462040`/`sub_4734F0` 落地后，传播冒出 2 处冲突：`sub_40C210` 同时被 `T_Font` 与 `T_Hashtable_int` 调用。
实测它的 `this` 只碰 **`+0x0`（缓冲指针）、`+0x10`（`_Mysize`）、`+0x14`（`_Myres`）** —— 这正是 **MSVC x86 `std::string` 的布局**
（`_Bx` 联合在 0、大小 0x10、容量 0x14），调用处也是 `v27[5]=…; v28=15; LOBYTE(v27[0])=0` 的 SSO 初始化。
⇒ 新类型 **`T_stdstring`（`adapter:true`，无 `size`/`complete`，非多态 ⇒ 无 RTTI 名）** + 载体 **c116**：15 站 **15 解释 / 0 残差**，冲突归零。

★ 这同时回答了"`+4` 那一族是不是语言基建"的另一半：**`RemoteDebug` 是游戏的**（写 `Error.log`），
**`std::string` 这种才是语言/STL 的**（无 vtable、无 RTTI 名，只能按偏移声明）。

## 诚实项（很重要的一条）

子代理做了**反事实实测**：把这 3 个 `T_Font` 判成别的类，解释变成 0/3/0、残差 186/171/161，而 **claims 仍然 exit 0、守卫全绿**
⇒ **"门禁全绿"不能给"判类"背书**，判类只能靠多判据交叉（这次用了 ①体内写 vtable（3 个都是 0 条，不提供信息）②偏移排他性 ③调用点 `this` 来路
④构造器见证（`Font`=`sub_464FD0`@`0x465007`、`Command`=`sub_415640`@`0x41567d`，其 `[this+K]` 与目标 obj0 位移集交 15/59 · 25/41 · 21/43））。
另一个坑：`T_Command+0x14D30` **内嵌了一个 `T_Font`**，所以"偏移落在成员跨度内"会给 `T_Command` **假命中**
（claims 报成 `⚪ 元素内偏移 0x1c6a4 不是 T_Font 的成员`，而 `0x1c6a4+0x14d30 = 0x313d4` 正是 Font 的成员）。

## 仍挂起的残差（下一步开批）

- `c112` 35 条 / `c113` 53 条 / `c114` 25 条 / `c115` 58 条 —— 全是**类型表缺口**，子代理已给出"前 30 条 off+宽度+站点 EA+逐字指令"。
- `c113` 里有 4 条是二级对象（宿主 `T_Command+0x4f11c` 未声明）。
- `c112` 里 3 条 `0x0` 是 `lea` **取址**（`T_Font` 没声明 `+0`，取址站点不算字段读）。

## 复现

`pnpm tools shape claims data/shape/carriers.json`（守恒① 6331 = 5798 + 533）· `pnpm tools shape propagate`（452 · 冲突 0）·
`pnpm tools shape calls`（1998 对 · 0 不一致）· `pnpm test`（398）。
