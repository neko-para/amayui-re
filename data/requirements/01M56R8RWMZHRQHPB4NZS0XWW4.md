# 批 S2v · shape claims --by-type：残差按载体类型聚合（把 .tmp 的临时脚本落成工具 + 守卫）

- id: REQ-01M56R8RWMZHRQHPB4NZS0XWW4
- type: req
- status: done
- parent: REQ-01M54KEF7X560CMPMGABG2W3CW
- verify: tools/test/shape-claims-guards.test.mjs#★㉖
- tags: [engine, shape]

# S2v · `shape claims --by-type`：残差按载体类型聚合 —— 已落

## 落点

`pnpm tools shape claims <file> --by-type` 在报告末尾多一节：

```
## 残差按类型（`--by-type`：**下一批该补哪个类型的成员表**；⛔ 不含位移不可用 / 未解析基址 / 数据段符号 ——
含 obj0 残差与二级对象残差，两者都表示"该类型的成员表缺这个偏移"）：N 处 / M 个类型
  T_Font                          41 个偏移 /  198 次访问　热点 0x574(28) 0x56c(25) …
```

实现：`renderByType(rep)`（`tools/lib/shape-claims.mjs`，`renderReport` 的 `byType` 选项）。

## 三条口径（都是踩出来的，写成注释 + 用例）

1. **类型从残差行自己取**，不从"当前处理到哪个载体"的状态取。`carriers.json` 的 id 不只有 `cNN`，
   还有按批发的 `b10cNN`/`b12cNN`；用 `^c\d+` 认 claims 的块头会把那些块漏掉、把残差记到**上一个**载体名下。
   实测代价：`T_Input` 8/8 与 `T_Sound` 4/5 被算成 `T_Font`（于是 `T_Font` 的派活清单多算 12 个偏移），
   `T_CBunki` 7/14 与 `T_ReadTextDB` 1/4 **整块没被派出去**。
2. **宿主可能是第二跳的类型**：`[this+K] → 指针 → [eax+M]` 的残差属于 `M` 的类型。
   用 `不是 <T> 的成员` / `落在内嵌子对象 <T>+` 从 `why` 里取 T；取不到才回落到载体类型。
3. 只统计 `⚪ 不落在…` 这一类；`位移不可用` / `未解析基址` / `数据段符号` 各是别的桶，⛔ 不许混进来。

## 守卫

- 用例：`tools/test/shape-claims-guards.test.mjs#★㉖`（两个反例：`b10c01` 这种批号载体要能归到正确类型 ·
  第二跳的残差要记到 TB 而不是 T0）
- 变异：`--by-type 不再从残差行取宿主类型（一律算成载体类型）`（112 → 113 条，全红）

## 复现

`pnpm tools shape claims data/shape/carriers.json --by-type`（当前实测见 `docs/02-engine/shape-census.md` §7.7「下一批怎么挑」）。
