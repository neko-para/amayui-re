# 批 S2a · batch-01/02 的裁决：T0 的内嵌子对象族（8 个基址）+ 内指规则 + 仍挂起的 7 条

- id: REQ-01M54MD3RQMEQV5D59Q9TXAXSS
- type: req
- status: done
- parent: REQ-01M54KEF7X560CMPMGABG2W3CW
- order: 53
- done_reason: 这四条挂起项已全部裁定并写进正文。S2b 五项实测结论：offsetUnusable 漏无索引 lea 不成立（探针 acc=a abs=0x300）；claims 与 access 对 sub_43E9F0 的差异是显示口径（274 站点多在数据段符号桶，载体行统计的是 obj0 相对解释数）；lea 索引丢失未修但评估为不划算（132 处受影响，而修法要改 1213 处 lea 的抽象解释）因此改走 axis 标注；rep movsd 后寄存器未失效已修（守卫 ★access-05/★access-06 + 变异 123/123，解释 8439 降到 7644 即 795 处假解释，差额 795 加 5 等于 800 可逐类解释）；coverage 的 call reg 未测，延后理由：它只影响 coverage 视图的口径，不进 claims 的任何守恒等式与判据，且 --by-type 全量检查现跑 0 处已作兜底；一旦 coverage 视图与 claims 数字对不上，它会以某载体访问数与站点数不符的形式暴露，届时再测。
- tags: [engine shape]

# 批 S2a · batch-01/02 的裁决：T0 的内嵌子对象族 + 内指规则 + 挂起项（本轮重新裁定）

- id: REQ-01M54MD3RQMEQV5D59Q9TXAXSS
- type: req
- parent: REQ-01M54KEF7X560CMPMGABG2W3CW
- order: 53
- tags: [engine shape]
- status: open

## 目标

batch-01…04 的裁决与挂起项。机制与证据细节在 `docs/02-engine/shape-census.md` §7.7（批次机制）/ §7.8（裁决记录）。
本单只留"现在是什么状态"。

## 已落地（本轮现跑复核）

`T_Command` 的内嵌族**已经是 27 个**（远多于原来记的 8 个），逐个都是 `{type, count:1}`：

```
0x408=T_Input  0x1ee8=T_Sys3Draw  0x48e8=T_Sound  0x511c=T_Data  0x55d8=T_CBunki  0xcac0=T_CBunki
0x13fa8=T_Hook 0x143bc=T_SE       0x148a0=T_Voice  0x14d30=T_Font  0x4e3ac=T_ReadTextDB
0x4e824=T_Renderer 0x4ed10=T_Scene 0x5d860/0x5d870=T_stdvector  0x68ea4=T_Stack_int
0x69250=T_stdvector 0x6927c=T_Hashtable_int 0x77960/0x77970=T_stdvector
0x77a3c=T_Queue_CACHE_ALPHADATA 0x796dc=T_Queue_int 0xa609c=T_FileDB 0xaa0e4=T_SysCursor
0xaa514=T_Reg 0xaa5d8=T_Music 0xaaa18=T_inihdr
```

## 挂起项 —— 本轮重新裁定（3 条已闭、2 条转成明确单）

| 原挂起项 | 现状（现跑核对） | 处置 |
|---|---|---|
| `T0+0x408/0x40c/0x414` 归属 | `0x408 = {type:T_Input, count:1}`（`size(T_Input)=0x1AE0` ⇒ 覆盖 `0x408..0x1EE8`，正好接上 `0x1ee8=T_Sys3Draw`）；`0x40c`/`0x414` 已**不存在**独立成员 | ✅ **已闭**：它们落在内嵌 `T_Input` 里 |
| `T_rec50` vs `T_a609c_elem` | `T_a609c_elem` **已不存在**；`T_rec50` 只被 `T_FileDB_3028` 引用 | ✅ **已闭**：`T_rec50` 是 `T_FileDB` 族里的记录类型，与 `T_a609c` 无关（`T_a609c` 现名 `T_FileDB`） |
| `T_143bc` vs `T_a609c` | `T_143bc` **已不存在**独立类型（`T_Command+0x143bc = T_SE[1]`） | ✅ **已闭** |
| `Engine+0x161C` 与 `T_408+0x1214`（同字段两套原点） | 两处都还在、都各自有载体 | ⬜ **保留两条互注**（这是**表达选择**，不是未判：同一个字段既可经 `T0+0x161C` 也可经 `T_Input+0x1214` 到达；两条声明互为注记，`calls`/`propagate` 均无冲突） |
| 守恒① 小差额（历史：1156→1154、残差 65→67） | 现跑 `守恒① 8439 = 8439 + 0`（残差 **0**）、守恒②③④ 全 ✅ | ✅ **已闭**：差额早被后续批次吸收，等式成立 |

## 仍要做（都不需要人，且已缩到两条）

1. **`Engine+0x161C` / `T_408+0x1214` 的互注**：在两条成员表的 `note` 里互相写一句（现在靠需求单记着，容易丢）。
   - 判据：两边各有一条 note 指向对方；**不**新增/删除声明。
2. **`S2b` 的助手待修项**是否已全部落地：`rep movsd` 后丢 `edx` · `offsetUnusable` 漏无索引 `lea` ·
   `lea reg,[base+idx*8]` 丢索引项 · `coverage` 把"寄存器←已知符号"的 `call reg` 记成目标未知 ·
   `--claims` 与 `--access --calibrated` 对 `sub_43E9F0` 不一致。
   - 其中前两条与本轮的 `lea [r+0]` 过滤、`memScale` 乘数链**相关**，要逐条实测确认（不能凭"感觉修过了"）。

## S2b 五项：逐条实测结果（本轮补 3 条）

| 项 | 结论 | 实测 |
|---|---|---|
| `offsetUnusable` 漏无索引 `lea` | **不成立（现状已覆盖）** | 探针 `.tmp/shape/probe-s2b2.mjs`：`lea eax,[esi+300h]`（无索引）⇒ `acc=a abs=0x300 prob=[]`，站点被记录、没漏 |
| `rep movsd` 之后寄存器没失效 | **未修（真问题）** | 同一探针：`rep movsd` 之后 `mov edx,[esi+400h]` 仍报 `acc=r abs=0x400 prob=[]` —— 但 `rep movsd` 会推进 `esi/edi`，之后的 `[esi+…]` 已不是对象相对位移 ⇒ 这里会产生**假解释** |
| `--claims` 与 `--access --calibrated` 对 `sub_43E9F0` 不一致 | **不成立（是显示口径，不是 bug）** | `access`：274 处站点（但 `base=absolute obj=dword_559A68` 占绝大多数）；`claims` 的载体行 `c74 sub_43E9F0 访问 0 处` 统计的是**该载体的 obj0 相对解释数**，那些站点进的是「数据段符号」桶（守恒③ 里单列）⇒ 两个数不是同一个量 |
| `lea reg,[base+idx*S+disp]` 丢索引项 | **未修（已评估：不值得按原计划修）** | 见 `S2ag`：132 处受影响，修法要改抽象解释 ⇒ 改走 `axis` 标注（已落地） |
| `coverage` 把"寄存器←已知符号"的 `call reg` 记成目标未知 | **本轮没测** | 不写成"已修" |

## 需要人裁决

**无。**

## 复现

```powershell
node -e "const t=require('./data/shape/types.json');console.log(Object.entries(t.types.T_Command.members).filter(([k,m])=>m.count===1&&m.type).map(([k,m])=>m.off+'='+m.type).join(' '))"
pnpm tools shape claims data/shape/carriers.json | rg "守恒①"
```
