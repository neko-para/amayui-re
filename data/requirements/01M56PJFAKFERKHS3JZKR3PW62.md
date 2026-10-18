# 批 S2s · 函数指针数组暴露的问题：模型（分派表数组化）+ 判定（数组窗口/元素内偏移 两个 bug）+ G15 + 撤回两条假缺陷

- id: REQ-01M56PJFAKFERKHS3JZKR3PW62
- type: req
- status: done
- parent: REQ-01M54KEF7X560CMPMGABG2W3CW
- verify: tools/test/shape-claims-guards.test.mjs#★㉒
- tags: [engine shape]

# S2s · 函数指针数组暴露的问题：**模型**与**判定**两处都已修

## 一、修好的（模型）

| 项 | 修前 | 修后 |
|---|---|---|
| **op 分派表** | `Command+0xa509c` **544 条离散内联成员**（543 条 `name:"fnptr"` + 1 条） | **1 条 `{off:"0xa509c", type:"T_fnptr4", count:1024}`**（`T_Command` 成员 965 → **422**）；`T_fnptr4` 补了 `+0x0 {width:4}`（元素内偏移 0 可解释） |
| 依据 | 3 个读点全是 `[esi+eax*4+0A509Ch]`（索引）；构造器 `0x415df6 lea edi,[esi+0A509Ch]` + `0x415e08` 起 `rep stosd` 打底 1024 项 + 逐槽覆写 544 项 | 数组末端 = `0xa509c + 4×1024 = 0xa609c`，与「1024 项」自洽 |

## 二、修好的（**判定** —— 这轮真正的收获：两个判定 bug）

数组化之后立刻暴露：`c92`（构造器）的解释数从 **879 掉到 336**、残差从 11 涨到 **554**。追下去是两个**独立**的判定 bug：

| # | bug | 位置 | 后果 | 修法 |
|---|---|---|---|---|
| ① | **有 `count` 的数组，窗口只覆盖第 1 个元素** | `windowsOf()`：`to = off + stride*d + elemSize`（`count` 被忽略） | 带索引的访问（落在数组起点）能对上；**不带索引、落在第 k>0 个元素**的访问（如 `mov [esi+0A50A0h], offset sub_418E60`）全部"不落在任何已声明成员上" ⇒ 残差 | `count` 已知 ⇒ 窗口覆盖 `[off, off + elemSize*count)`；`count:null`（sweep）**保持**单元素窗口（无界窗口必须靠索引身份约束）。★ 视图窗口也保持单元素（否则相邻视图互相"观察"，把 G9 的「零观察」判据搅乱 —— 实测 ★⑥ 会红） |
| ② | **元素内偏移不取模** | G2 的数组分支：`elemOff = absDisp - from` | 「第 k 个元素的第 0 个字段」被算成「元素 0 的偏移 k*stride」⇒ 只有 k=0 能对上 | 未带索引 ⇒ `(absDisp - from) % elemSize`；带索引 ⇒ 用差值（索引已记在 `indexSym`/`stride`） |

修完：`c92` **889 站 = 880 解释 + 10 残差**（数组化前是 879/11）· 全模型守恒① **6498 = 5964 + 534**（数组化前 5798 + 533）。
守卫 **★㉓** + 变异 **2 条**（拆掉任一个 bug 的修法都当场红）。

## 三、新义务 G15（让这类矛盾"可见"）

索引访问落在**非数组**成员上 ⇒ 逐条点名 EA（"这些成员应当声明成数组"）；落在**数组**上 ⇒ note（"形态已被数组承住"）；
位移 0 + 未解析索引 / 未声明偏移 ⇒ 跳过（留在「位移不可用」桶）。当前 **note 级**（100+ 处命中，多是还没建完的类型表），
⛔ 不算通过；守卫 ★㉒ + 变异 1 条。翻红的前置写在"重开条件"里。

★ 反事实：**只改表示法不改判据，`findings` 仍然 = 0** —— 所以模型与判据必须**一起**改。

## 四、撤回两条假缺陷（调研报告里的 ②③）

`mov dword ptr [ecx+1FC4h], offset sub_418E30` **看起来**在偏移 0x1fc4 上写，但 `ecx` 是
`0x41795A lea ecx,[esi+0A30D8h]` 之后的**表指针**（`0x4179CC add ecx,10h`、`0x4179CF dec ebx` `ebx=0x100`），
而 `0xa30d8 + 0x1fc4 = 0xa509c` ⇒ 这 4 条写**就是分派表每 4 个槽的条件复位**。

- 缺陷②「`T_Command+0x1fc4..0x1fd0` 无成员」**撤回**（真身是 `0xa509c+16k`）。
- 缺陷③「`T_Sys3Draw` 坐标不自洽」**撤回**（两条前提都是这次误读）。

★ 教训已写进 `docs/02-engine/shape-census.md`「指令文本里的位移 ≠ 折叠后的位移」：
判断偏移属于谁**必须看 `shape access` 的 `abs=`**，不许读指令文本里的位移。

## 五、待裁决（不猜）

1. **`Command+0xa30d8` 仍是 183 条离散成员**：已知 `lea ecx,[esi+0A30D8h]`、0x1000 字节、`sub_417800` 按 16 字节步进 256 次、
   末端 `0xa40d8` 被引用 **72** 次（G4 的 count 有着落）；但**元素语义没定**（构造器往 `0xa3100+` 写的是 `&table` 一类的值）
   ⇒ 硬写 `count:1024` 会造出语义错误的窗口。
2. **G15 翻红**：前置 = 本单第五节 1 + `T_FileDB+0x3c48`（S2m）都裁决完。
3. `sub_411900`（另一个分派读点的宿主）**仍无载体**。

## 六、函数指针"传播"不实现（用户口径）

数量有限、用到时再查。派生视图在 `.tmp/shape/fnptr/inventory.json`：**1024 项 opcode → handler 全部解出**
（544 `immediate` + 480 `loop` 默认 `sub_418E30`），`opcodeMap` **永不进 `types.json`**。

## 复现

`pnpm tools shape claims data/shape/carriers.json`（守恒① 6498 = 5964 + 534 · G15 段）· `pnpm test`（400）·
`pnpm tools mutate check`（109/109）· `pnpm tools shape access sub_417800`（那 4 条写是 `base=unresolved`，因为 `ecx` 是循环表指针）。
