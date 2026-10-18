# 批 S2k · T_Scene 成员表（构造器 55 条强档 · 拒合弱档 4 条）+ 两数组升 count:1000 + 一条归属订正

- id: REQ-01M55HYEDKQ7GBNJ8052JHWJEY
- type: req
- status: done
- parent: REQ-01M54KEF7X560CMPMGABG2W3CW
- done_reason: 数据类合并：55 条只取构造器逐条写到的偏移（弱档 4 条拒合，实测会变假绿）；由 claims 逐载体 explained/residue 与六条门禁背书
- tags: [engine shape]

# S2k · `T_Scene` 成员表（构造器 55 条强档）+ 两个数组升 `count:1000` + 一条归属订正

`T_Scene`（RTTI `??_7Scene@@6B@`，构造器 `sub_4AAF90`，vtable 写入 `0x4aafcd`，`Command+0x4ED10` 的内嵌子对象）
原来只有 **13 条成员** ⇒ 3 个 `T_Scene` 载体解释率极低（3/23、3/13、3/8）。

## 合并了什么

- **强档 55 条**（`types-patch.json`）：**只含构造器逐条写到的偏移** —— vtable `0x0`@`0x4aafcd`、
  容器槽 `0x40c/0x410/0x41c/0x420/0x430/0x43c/0x440/0x44c/0x450`、`0x744`(ptr,type:null，c99 有解引用)、
  `0xa5d4`、`0xb578…0xb5a4` 一带、`0xc610`、**`0xc614` = `T2×1000`**（`rep stosd`@`0x4ab236`，窗口 `[0xc614,0xd5b4)`）、
  `0xd5b4…0xd5d8`、`0xd9ec/0xd9f0/0xd9f8/0xd9fc/0xda00`（三个 `fstp dword ptr`）、`0xda04…0xda28`；另加 `T_Scene_42c+0x4c`。
- `T_Scene` 现有 **69 条成员**。
- ⛔ **没有**合并 `types-patch-weak.json`（4 条构造器一行没写、只有多函数共用）—— 实测合并后
  c99/c100/c102 会变成 19/0、16/0、11/0，**那是假绿**（c99 仍有 7 处二级不透明）。

## 两处**订正**（提案点名 + 我按证据落地）

1. **`0xa5d8` / `0xb670`：`count:null` + `sweep` → `count:1000`（并去掉 `sweep`）**。
   证据：构造器一个 1000 轮递减循环清出 `[0xa5d8,0xb578)` 与 `[0xb670,0xc610)`（`mov ecx,3E7h`@`0x4ab13f`），
   两个末端 `0xb578`/`0xc610` **都被构造器直接写入** ⇒ G4 可判。
   ★ 踩过：count 与 sweep 不能并存（G8 当场红：`count 已给出条数 ⇒ 不该再给 sweep`）。
2. **`T_Command+0x5a37c` → `T_Scene+0xb66c`**：该偏移落在 `T_Scene` 跨度 `[0x4ed10, 0x4ed10+0xda30)` 内
   （`0x4ed10+0xda30 = 0x5c740` 正好是 1000 项分派表的起点）⇒ 归属给 `T_Scene`（同 batch-07b 的口径）。

## 实测（合并后）

| 载体 | 合并前 | 合并后 |
|---|---|---|
| `c99 sub_49AA30` | 3 / 23 | **10 / 9**（二级 7 全不透明） |
| `c100 sub_49CC10` | 3 / 13 | **4 / 12**（位移不可用 6） |
| `c102 sub_4A07F0` | 3 / 8 | **4 / 7**（位移不可用 4） |

## 诚实项

- **内嵌子对象 0 个**：构造器体内没有 `lea ecx,[this+K]` + `call ctor`；5 个容器（`0x408/0x418/0x428/0x438/0x448`）
  是**内联构造**（`lea edi` + `operator new` 0x2f8/0x74/0x50/0x250/0x250 + 自环 + 两个 byte 标志），节点块无 RTTI 名
  ⇒ 目标类型全 `null`。**容器基址格没声明**（构造器只 `lea` 不读写 ⇒ 声明它会是零观察成员）；
  代价：`sub_4AB7A0`(c31) 的 4 个 `lea` 站点仍是残差。
- **`size` 仍是区间 `[0xda2c,0xda30)`**：lo 由构造器写入上界确证（`mov [esi+0DA28h],ebx`@`0x4ab26e`，+4）；hi 零观察。
- 4 个节点槽进 **G7 空话声明**（ptr 但本模型零解引用、type 只能 `null`）—— G7 从 24 涨到 **44**，⛔ 不算通过。
- `sub_4B06D0` **不在载体里**（只在 propagate 的 inferred，hops=2）⇒ 所谓"4 个 weak 载体"实际只有 3 个能列残差。

## 复现

`pnpm tools shape claims data/shape/carriers.json`（c99 10/9 · c100 4/12 · c102 4/7）· `pnpm test`（398）·
`node .tmp/shape/batch-09/scene/build.mjs`（确定性，两跑同哈希）。
