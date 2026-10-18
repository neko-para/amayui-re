# 批 S2ag · 复杂度清理 + lea 索引丢失改走 axis 标注

- id: REQ-01M57QGSN2A07XY4PHDRDH3020
- type: req
- status: done
- parent: REQ-01M54KEF7X560CMPMGABG2W3CW
- done_reason: axis 标注从 1 例扩到 4 例：T_Command+0x5D880（0x40F3A3..0x40F3B1 强度削减 15x8=0x78=size(T1)）+ 0x5D7E8/0x5D804/0x5D82C（游标块/条数块按 T1 元素 120 字节推进，依据 0x41AAD9..0x41AB66 换槽与 0x41513F..0x415189 复制）。G17 守卫（axis.stride 必须有同容器已声明数组背书）+ ★㉜ 三条断言 + 变异「不再要求 axis 有备份」已落地。判据与边界写进 docs/02-engine/shape-census.md 的「axis 标注（S2ag）」一节：能标的是判断（哪个槽是游标、步长多少），不能标机械可读事实（末尾引用/宽度/站点清单）。清理两项亦已在本单完成：G15 五桶合并为 classifyIndexSite()、resolveDeep 去魔法数，均以 claims --by-type 输出 sha256 不变验收
- tags: [engine, shape]

## 一、已做的清理（行为不变）

**删掉负 pointee 的死分支**（8 行 → 2 行注释）：`shape-access` 已把负 pointee 偏移折成 `null`（守卫 `★access-01`），
claims 里 `if (pointeeOff < 0)` 永不成立，且与「没有可用二级偏移」那档同义 —— 第二套措辞。

- 判据：**同一快照 `claims --by-type` 输出 sha256 前后相同**（`24C367DD1B90823F`）⇒ 行为零变化；`pnpm test 412/412`。
- 未做：G15 五桶合并成一个分类器（−60~80 行，要动 24 处引用，性价比不划算）。

## 二、`lea reg,[base+idx*S+disp]` 丢索引：不值得按原计划修

实测（`probe-s2b.mjs`）：`403004/40300b` 的 `stride=0`、`indexSym=null`（`fold` 却看得见 `[edx*8]`）。

```
lea 带索引        1213 处
  再叠位移         132 处   ← 只有这些会丢索引项
mov 带索引+位移   1048 处   ← 不丢（走 mem.indices）
```

⇒ 受影响 132 处；原计划要改**抽象解释的地址值表示**、牵动全部 1213 处 `lea` ⇒ 不划算。改走 `axis` 标注。

## 三、进展：`axis` 标注已落地（G17）

- 数据：`T_Command+0x5D880` 加 `axis {stride:120, index:'var', why:'0x40F3A3..0x40F3B1 强度削减 15×8=0x78'}`。
  ★ **设计修正**：原计划标在 `0x5D8BC`，但它落在 `T1×40` 的数组跨度内（不是独立成员，无法标注）；
  真正该标的是**游标槽** `0x5D880`（`0x40F3A4` 读它当索引）。
- 判定器：新增 **G17** —— `axis` 必须是 `{stride>0, index, why≥8字符}`，且 `axis.stride` 必须有**已声明数组**背书
  （同容器内存在数组成员且元素大小 == stride），否则红。实例：`120 == size(T1)` 且 `0x5d894 = T1×40` ⇒ 通过。
- 门禁：`claims exit 0`（8439 = 8439 + 0）· `test 412/412` · `mutation 121/121` · `requirements 5/5`。
- ✅ **已做**：G17 的合成用例 `★㉜`（三条断言：合法不红 / 无背书红 / why 太短红）+ 变异「不再要求 axis.stride 有备份」（`mutation 121 → 122/122`，`test 412 → 413`）。

## 四、清理进展

- ✅ **清理 2 已完成**：`resolveDeep` 的深度上限不再写死 3，改用**模型实际嵌套深度**（obj0 起按 `{type}` 递归算，带环保护）。
  验收：`claims --by-type` 输出 sha256 前后相同（`24C367DD1B90823F`）⇒ 行为零变化；`test 413/413`、`mutation 122/122`。
  实测模型嵌套深度上限 = **4** —— 写死 3 时第 4 层会被静默当成"判不了"。

## 五、剩下的小清理（不另开单）

1. G15 五桶 → 一个 `classifyIndexSite()` + 渲染器（−60~80 行）
2. ~~`resolveDeep` 的深度 3 魔法数~~ ⇒ 已在上面的清理 2 完成
3. `S2a` 里 `S2b` 另外 4 项助手待修：仍未测（`rep movsd` 丢 `edx`、`offsetUnusable` 漏无索引 `lea`、
   `coverage` 的 `call reg`、`sub_43E9F0` 的 claims/access 不一致）
