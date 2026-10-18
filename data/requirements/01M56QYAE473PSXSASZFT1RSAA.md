# 批 S2u · batch-13：按残差落点分活收口（T_Font 199 · T_Command 158 · Sys3Draw/Scene/stdstring）

- id: REQ-01M56QYAE473PSXSASZFT1RSAA
- type: req
- status: done
- parent: REQ-01M54KEF7X560CMPMGABG2W3CW
- done_reason: 四份提案全部合并（render-scene-str/command/font/small-types + batch-15 empty-types）：claims 残差 500→53（解释 6445/6498 = 99.2%），守恒①②③④ 自洽、exit 0；propagate 冲突 0、calls 不一致 0、pnpm test 403/403、变异 113/113
- tags: [engine, shape]

# S2u · batch-13：按残差落点分活收口 —— **已完成 4/4 合并**

## 为什么这批这么分（方法论，已写进 `shape-census` §7.7）

不再"按未标函数排名"，改成**按残差落点分活**：把 `claims` 里"访问 0x… 不落在任何已声明成员上"的站点按
**载体类型**聚合 ⇒ 得到"哪个类型的成员表还缺哪些偏移、被哪些函数碰"。收益可度量（残差访问数下降），
派活自带上下文，而且它自己会暴露模型缺陷。

★ 聚合的**归因 bug**（已修，落成 `--by-type`）：块头正则只认 `c\d+|g\d+`，`b10c*`/`b12c*` 这些按批发的 id
匹配不上 ⇒ 残差被记到**上一个**匹配到的载体名下。后果：`T_Font` 的派活清单多算 12 个偏移
（44/199 vs 真实 32/186，多出来的是 `T_Input` 8/8 与 `T_Sound` 4/5），而 `T_CBunki` 7/14 与 `T_ReadTextDB` 1/4
**整块没被派出去**。⇒ 修法：类型从**残差行自带**的 `不落在 *T_X*` / `不是 T_X 的成员` 取（`★㉖` 守着）。

## 四份都合了（每份都自检过、我复跑过门禁）

| 份 | 内容 | 效果（本模型） |
|---|---|---|
| `render-scene-str/` | 17 条（`T_Sys3Draw` 7 漏槽 · `T_Scene` 8 · 两个 `+0xc` 分配器槽） | 残差 500 → 428 |
| `command/` | 65 条（`T_Command` 38 声明的 47 偏移 · `T_Scene`/`T_Renderer` size · `T_Sys3Draw` 13） | 428 → 310 |
| `command/` §5.3 归属澄清 | 两个内嵌子对象定 size（`T_Sys3Draw` 0x2a00 / `T_Input` 0x1ae0 = 与相邻内嵌的偏移差）+ 删 `T_Command` 里 22 条**重复声明** + 12 条搬进 `T_Sys3Draw` | 310 → 270 |
| `font/` | 33 条（19 标量 · 2 ptr · 3 个 `count:4096` 数组 + 新元素类型 `T_1cf2c_elem`(20B)） | 270 → 84 |
| `small-types/` | 28 条（`T_CBunki` 5 数组 · `T_Input` `0x1194` 32 数组 · `T_Sound` 3 数组 + 新 `T_Sound_690` · `T_ReadTextDB+0x8` 1024 缓冲） | 84 → **53** |
| `empty-types/`（batch-15） | 86 条（`T_Renderer` 42 · `T_Music` 11 · `T_Queue_CACHE_ALPHADATA` 7 · `T_Reg` 9（删 4 条父级错归属）+ 2 个元素类型） | 数字不变（这四个类型还没有载体） |

**残差：500 → 53**（解释 5964 → 6445，96.8% → **99.2%**）。

## 合并时处理掉的三处真冲突

1. **`T_Input+0x1198` vs `0x1194`**：`T_Command` 那份把它当独立标量，`small-types` 独立证明
   `0x1194 = T_el32×32`（循环立即数 `20h` + 块长 `0x84` + 末尾 `0x1214` 已声明），而 `+1198h` 全语料**零命中**
   ⇒ 删掉 `0x1198`（G1 红当场出现又消失）。
2. **`T_Reg+0x4` 同时带 `width` 与 `type+count`**（G8 红）：合并工具把"内联值改成内嵌子对象"时的 `width` 留了下来
   ⇒ 合并脚本加"`null` = 删键"。
3. **`T_Reg` 原有的 `0x8/0xc/0x14/0x1c` 是子对象字段的错归属**（逐字等于 `0x4` 那个 Hashtable 的成员平移 4）
   ⇒ 删（否则与 `0x4(+80)` 重叠）。

## 复核过的反例（都是"看着像数组/指针，实测不是"）

- `T_Scene+0xb5a4` 全语料 63 次全是裸位移、无索引 ⇒ **标量**（有符号整数）。
- `T_Sys3Draw+0x1fe0..0x1ffc` 是两条 **16 字节按值记录**（整块搬栈）⇒ 4 个 dword 单槽，不是数组。
- `T_Music+0x440` 从未被写，只是 `0x434+3*4` 的**折叠基址** ⇒ 不是成员。
- `T_CBunki+0x8c4` / `T_Sound+0x7e0` 都不是成员，分别是 `0x8bc+8` / `0x690+14*0x18`（元素内偏移）。
- `T_Command+0x1e74` 不是 `T_Command` 自己的（10 处站点全是 `[obj0+0x408]+0x1a6c`）⇒ 属 `T_Input`；
  在 `T_Input.size` 落定后它被自动修正，不需要假声明。
- `T_stdstring`/c116 **不是错类**（18 个站点只有 `+0x0/+0x10/+0x14`）；"疑似错类"是聚合归因假象。

## 剩下的 53 处（下一批的活，已按类型点名）

`T1` 13/41（元素成员洞）· `T_Command` 5/28（含数组元素内宿主，见 S2x）· `T_Font` 6/12
（`0x0` 全是 `lea [r+0]`，是工具缺口 S2x①）· `T_5c73c_0` 5/5 · `T_ReadTextDB` 2/3 · `T_Data` 1/1。

## 验收（本轮实跑）

`claims` exit 0（`6498 = 6445 + 53`，守恒①②③④ 自洽）· `propagate` 471 函数 / 冲突 0 ·
`calls` 2068 对 / 不一致 0 · `pnpm test` 403/403 · `test:mutation` 113/113 · `requirements validate` 5/5。
