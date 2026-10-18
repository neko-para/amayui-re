# 批 S2aa · 宿主落在容器里（数组元素/内嵌子对象/套娃）：G5' 宿主的递归解析

- id: REQ-01M56V6SX3VC56FVXHB4ZYC8MA
- type: req
- status: done
- parent: REQ-01M54KEF7X560CMPMGABG2W3CW
- done_reason: resolveDeep 递归解析容器内宿主（ptr 成员不算容器、元素首双字=vtable 记不透明、非 0 偏移标量当基址记残差）；二级'未声明成员'残差 35→2，T_Command 的 0x59380/0x4f11c 归位；变异 118 条全红
- tags: [engine, shape]

# S2aa · 宿主落在**容器里**（数组元素 / 内嵌子对象 / 两者套着）—— 已落

## 问题

`G5'` 找"二级对象的宿主"时只做**精确 `off` 相等**，于是宿主的 obj0 相对偏移一旦落在**容器内部**就判不了：

- `T_Command+0x5d904` / `+0x5d908` = `T1[40]` 元素内 `+0x70` / `+0x74`
- `T_Command+0x4f11c` = 内嵌 `T_Scene`（`T_Command+0x4ed10`）的 `+0x40c`
- `T_Command+0x59380` = `T_Scene+0xa670`（还要再穿过 `T_Scene+0xa5d8 = T2[1000]` 落到 `T2+0x0`）
- `T_Scene+0xa670` 本身 = `T2[1000]` 元素 `+0x0`

## 判据（一条递归解析，取代原来的三条分支）

`resolveDeep(hop, typeId)`：在本类型的成员里找**覆盖 hop 的容器成员**（`type` + 元素 `size`，且不是 `kind:"ptr"`）⇒
换算成元素/子对象内偏移 `rel = (hop - off) % size` ⇒ 到那个类型里再找一层（最多 3 层）。
命中标量就返回；命中"容器套容器"继续下钻；中途缺成员 ⇒ 残差（措辞写清"落在 X 的容器（+off）内偏移 rel 里"）。

两条配套规则（都是踩出来的）：

1. **`kind:"ptr"` 成员不是容器**：否则 `T_Sys3Draw+0x1fb0`（ptr→`T_Scene`）会被当作 `T_Scene[1]` 的窗口，
   把它后面的 0xDA2C 字节全吞了（实测凭空多 17 处残差）。
2. **元素/子对象的第一双字 = vtable**：`elemOff === 0` 且该成员不是 ptr ⇒ 解引用的是**虚表**（虚调用），
   记**不透明**（⛔ 不算通过），不判红。元素内**非 0 偏移**的标量被当基址用 ⇒ 记**残差**
   （那是"元素类型还缺一条 `kind:'ptr'`"的缺口，不是这条载体声明错）。

## 效果

- `未声明成员` 的二级残差 35 → 2；`T_Command` 的 0x59380/0x4f11c 归位；`T_Scene+0xa670` 转成"元素 vtable ⇒ 不透明"
- 守恒② 分档随之上移（`不透明` ↑ / `拒绝未声明` ↓），总数不变

## 守卫

- 变异：`指针成员被当成容器`（`★` 断言靠 `T_Sys3Draw+0x1fb0` 那种形态抓）
- 说明：这一档是"解析能力"，不是"新义务"，所以只加变异、不新增红档

## 复现

`pnpm tools shape claims data/shape/carriers.json --by-type`（`T_Command` 不再出现 `0x59380`/`0x4f11c`）。
