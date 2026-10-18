# 批 S2n · T_Input/T_Data/T_Sound 成员表（+133 成员 · 34 载体）+ BaseObject 的 vtable 初始化器（冲突 3→1）

- id: REQ-01M55KC6QAZ34E8YQ43WV74RNG
- type: req
- status: done
- parent: REQ-01M54KEF7X560CMPMGABG2W3CW
- done_reason: 数据类合并：三个内嵌子对象成员表 + 34 条载体 + BaseObject vtable 初始化器载体；由 claims 守恒①/②/③、calls 0 不一致与 propagate 冲突 3→1 背书
- tags: [engine shape]

# S2n · `T_Input` / `T_Data` / `T_Sound` 三个内嵌子对象（+133 成员 · 34 条载体）+ `BaseObject` 的 vtable 初始化器

三个 `T_Command` 内嵌子对象原来成员表都很薄，导致它们的函数几乎解释不了。本单按"构造器为主、站点补盲"补表，
并把 34 条已定类函数的载体落地。

## 结果

| 类型 | 成员 | size | 载体 |
|---|---|---|---|
| `T_Input`（`+0x408`，ctor `sub_477DD0`） | 6 → **127** | 只有**下界** 0x1adc（区间 `[0x1adc,0x1ae0)`） | 若干（含 `sub_476AA0`：93 站全解释） |
| `T_Data`（`+0x511c`，ctor `sub_4388C0`） | 5 → **9** | **0x4bc**（lo==hi：写入上界与相邻 K 差一点） | 若干 |
| `T_Sound`（`+0x48e8`，ctor `sub_4B5B50`） | 0 → **17** | **0x834**（同上） | 若干 |

- **内嵌子对象 2 个**（都在 `T_Data`）：`+0x414` = `Hashtable<int>`（`sub_415460`，`??_7?$Hashtable@H@@6B@`）、
  `+0x464` = `Hashtable<std::string>`（`sub_4387F0`，新类型 `T_Hashtable_string`）；两者 `size` 都由相邻 K 差钉死 = **0x50**。
  `T_Input`/`T_Sound` 构造器里**没有** `lea ecx,[this+K]`+`call` 形态。
- **新发现**：`T_Data+0x8` / `T_Sound+0x8` 是 **0x400 字节字符缓冲**
  （`T_Data` 有 3 处 `sprintf_s(Buffer=this+8, BufferCount=0x400)`：`0x437558/0x437aae/0x43820e`；`T_Sound` 6 处 `wsprintfA` 目的；
  `0x8+0x400=0x408` 恰好接下一个成员）。`T_Data` 布局**完全闭合**：`0x0/0x4` 基类 8 + `0x8` 缓冲 0x400 + `0x408…0x4b8`。
- **1 处订正**：`T_Input+0x1a74` `width 1 → 4`（`0x477f7c mov [esi+1A74h], edi` 是 dword 存；`0x477585 test byte ptr` 是**窄读**）。

## 顺带修掉的传播冲突：`sub_4034B0`（`BaseObject` 的 vtable 初始化器）

它是**一条指令**：`0x4034b0 mov dword ptr [ecx], offset ??_7BaseObject@@6B@`，**29 个调用方**（各类构造器里先"装基类 vtable"）。
新载体 **c110 `sub_4034B0 → T_BaseObject*`** ⇒ 传播冲突从 **3 → 1**（只剩 `sub_4BB810`，见 S2m①），
可推导函数 **390 → 417**。

## 实测

- 既有 6 条载体（`c40/c70/c71/c72/c16/c41`）的 explained/残差**完全无变化**；变化全在 34 条新载体上：
  `sub_476AA0` 93/93（残差 0）· `sub_477DD0` 48/51 · `sub_415460` 28/28 · `sub_4387F0` 28/28 · `sub_437980` 26/26。
- 全模型：守恒① **5455 = 5086 + 369**、`calls` 1662 对 **0 不一致**、G7 空话声明 53 条（⛔ 不算通过）。

## 诚实项

- `T_Input` 的 `size` **没定**（区间只差 4 字节）⇒ `T_Command+0x408` 的 G8 note("元素 size 未知")仍在。
  `size_lower` 只是文档字段，校验器不认。
- `T_Sound` 构造器有一个 **15 轮递减填循环**（`0x4b5baf lea esi,[edi+554h]` + `0x4b5bec sub esi,4`）：解成 6 个 dword 数组；
  其中 `0x654` 那条的末尾 `0x690` **零引用** ⇒ 按 G4 写 `count:null` + `sweep`（判不了，不是"通过"）。
- `T_Sound+0x8` 的 `count=0x400` 是**推断**（跨类一致 + 空洞恰好，强度中）。
- `T_Sound+0x408/0x40c/0x410` 被解引用 ⇒ 必须 `kind:"ptr"`（否则 G5' 红），但它们的目标类型无证据 ⇒ 落**不透明桶**。
- 弱档 5 条（`T_Input 0x1a8c/0x1a90/0x1a98/0x1a9a`、`T_Sound 0x420`）**没有合并**（单处非构造期观察，合并会假绿）。
- `sub_4B6440` 因 `.c` 约定查不到对象根寄存器而**不入账**；`sub_418520` 虽在 lea 名单里但位移到 `0x2450`（远超 0x834）
  ⇒ **不是** `T_Sound` 方法（"名单不能照抄"的反例）。
- 6 条 witness 未复核：都是 `T_Sound` 递减循环的写（基址每轮 −4 ⇒ 形状层解不出），独立证据是轮数 15 + `InitializeCriticalSection` 的 −0x18 交叉检验。

## 复现

`pnpm tools shape claims data/shape/carriers.json`（守恒① 5455 = 5086 + 369）· `pnpm tools shape propagate`（417 · 冲突 1）·
`pnpm tools shape calls`（1662 对 · 0 不一致）· `pnpm test`（398）· `node .tmp/shape/batch-10/input-data-sound/build.mjs --check`。
