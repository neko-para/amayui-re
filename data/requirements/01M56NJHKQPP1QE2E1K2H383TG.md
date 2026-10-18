# 批 S2q · T_CBunki/T_Hook/T_ReadTextDB（+17 成员 · 27 载体）+ 第 3 类"同形布局"：ICF 折叠的模板实例（T_Hashtable_layout）

- id: REQ-01M56NJHKQPP1QE2E1K2H383TG
- type: req
- status: done
- parent: REQ-01M54KEF7X560CMPMGABG2W3CW
- done_reason: 数据类合并：三个内嵌子对象 + 27 条载体 + ICF 同形布局适配器；由 claims 全绿、propagate 冲突 1→0、calls 0 不一致与逐载体实测 after 背书
- tags: [engine shape]

# S2q · `T_CBunki` / `T_Hook` / `T_ReadTextDB`（+17 成员 · 27 条载体）+ 第 3 类"同形布局"：**ICF 折叠的模板实例**

## 结果

| 类型 | 成员 | `size` | 备注 |
|---|---|---|---|
| `T_CBunki`（`+0x55d8` 与 `+0xcac0` 两个实例） | 11 → **20** | 29928 (0x74e8) | 新增 `0x74bc..0x74e4`、**`0x74d0` 宽 16**（来自载体集 4 个连续 dword 站点，⛔ 不是靠 `SetRectEmpty` 契约猜） |
| `T_Hook`（`+0x13fa8`） | 0 → **4** | 0x414 | `0x0` + `0x408/0x40c/0x410` |
| `T_ReadTextDB`（`+0x4e3ac`） | 0 → **4** | 0x478 | `0x0` + **两个内嵌子对象**（见下）+ `0x474` |

**内嵌子对象 2 个**（都在 `T_ReadTextDB`）：`T_Hashtable_READTEXTDB_FILEFLAG`（`+0x408`，size 0x50，18 成员）·
`T_Queue_READTEXTDB_BUFFER`（`+0x458`，size 0x1c，6 成员）。类名来自**指令自带的 vtable 符号**；
成员偏移与现仓 `T_Hashtable_int` / `T_Queue_int` **逐条同形**、size 也相同（交叉检验）。

## `T_CBunki` 两实例的交叉检验

`+0x55d8` 与 `+0xcac0` 共 **28 个载体**（含 vtable 虚函数 `sub_403FF0`）、**108 个 obj0 站点**，
最大相对偏移 `0x74e4 < 0x74e8`，**≥ size 的站点 0 个** ⇒ **一套相对偏移对两个实例同时成立**，没有建第二套类型。

## 载体 27 条（`b12c01..b12c27`）

判据是机械的：`lea ecx,[Command+K]` + `call`，以及 callee 全部 call site 的反向切片
（含 callee-saved 跨 `call`、`add ecx,K` 两种形态）都落在该槽上。
实测（子代理）：受影响 28 个载体的解释数 **7 → 139（强档）**；前 5：`sub_403C50` 15/1→**16/0** · `sub_404E20` 14/0 ·
`sub_403F40` 14/0 · `sub_403DD0` 12/0 · `sub_48FAF0` 11/0。

## 第 3 类共用实现：**ICF 折叠的模板实例** → `T_Hashtable_layout`

27 条载体落地后传播冒出 1 处冲突：`sub_429020` 同时被 `T_Hashtable_int` 与 `T_Hashtable_READTEXTDB_FILEFLAG` 调用。
两者是 **同一个模板的两个实例、成员逐条同形、size 都是 0x50** ⇒ MSVC 的**同形代码折叠（ICF）**让一份代码服务两个实例。
⇒ 新类型 **`T_Hashtable_layout`（`adapter:true`，复制 18 条同形成员，⛔ 不写 `size`/`complete`）** + 载体 **c117**：2 站全解释，冲突归零。

★ 这一格与之前两类要分清：**基类前缀**（`BaseObject`/`DrawEffect`，偏移 0 的继承）· **布局适配器**（`sub_4BB810` 的 `T_408_trio`，
两个**不同的类**在同一偏移上有同形字段）· **ICF 同形折叠**（本条的模板实例）。三者都表现为"同一函数被多种类调用"，但原因不同。

## 诚实项

- `T_ReadTextDB` 的弱档残差 3 条是"二级对象宿主"（`0x48ed14 / 0x48ffde / 0x48ffe0`）⇒ **不是全绿**；弱档 4 条单独成文件、未合并。
- `0x40c/0x59c/0x72c/0x7304` 是**运行期索引**（`idx=*(obj0+0x408)` / `*(obj0+0xefc)`），无立即数上界 ⇒ 不声明 `count`；
  本轮 `sweep` 声明 **0 处**（`count:null+sweep` 会把非索引的 `lea` 站点挤出解释范围 —— 实测取舍）。
- `0x8bc` 写成 **16 字节内联值**而不是数组（同上理由）。
- 内嵌 Hashtable 的 `+0x0` 写成内联 4B，**与现仓 `T_Hashtable_int+0x0`（`kind:"ptr"`）口径不一致** —— 只登记、未改。
- `+0x4` 属 `T_BaseObject`，三个子类型上不重复声明（三个构造器都 `call sub_4034A0`；RTTI 基类数组都是 2 条）。
- ★ 子代理做了一次**反事实实验**：给 `Hashtable+0x28` / `Queue+0x4` 加 `kind:"ptr"` ⇒ 解释数 **5237 → 5237 一点没变**（只把 3 条站点搬进不透明桶）⇒ **不采纳**。
- 基线是**冻结快照**（`_input/` + MANIFEST sha256）；现仓已被并发写过，对照在不参与 `--check` 的 `logs/live-drift.txt`。

## 复现

`pnpm tools shape claims data/shape/carriers.json`（守恒① 6496 = 5945 + 551）· `pnpm tools shape propagate`（471 · 冲突 0）·
`pnpm tools shape calls`（2068 对 · 0 不一致）· `pnpm test`（398）· `node .tmp/shape/batch-12/cbunki-hook-readtextdb/build.mjs --check`。
