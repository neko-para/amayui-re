# 批 S2o · T_SE/T_Voice/T_SysCursor 成员表 + 用 RTTI 基类数组定案 sub_4BB810（传播冲突 1→0）

- id: REQ-01M55KKEDG8K8TGV4WDGCGE9RZ
- type: req
- status: done
- parent: REQ-01M54KEF7X560CMPMGABG2W3CW
- done_reason: 数据类合并 + 一条新判定手段（RTTI 基类数组）：三张成员表 + 适配器 T_408_trio 载体；由 propagate 冲突 1→0、claims 全绿与 calls 0 不一致背书
- tags: [engine shape]

# S2o · `T_SE` / `T_Voice` / `T_SysCursor` 成员表 + 用 **RTTI 基类数组** 定案 `sub_4BB810`（冲突 1 → 0）

三张成员表补齐，并把这轮唯一挂着的传播冲突用一条**新判据**定案。

## 结果

| 类型 | 成员 | `size` |
|---|---|---|
| `T_SE`（`+0x143bc`，ctor `sub_4B4E60`） | 11 → **29** | **0x4e4**（构造器写入上界独立确认：`0x4e0+4`） |
| `T_Voice`（`+0x148a0`，ctor `sub_4BB700`） | 0 → **17** | **0x484**（我按 lo 定；hi `0x490` 是下一个子对象的 this 槽，中间 12B **零观察**） |
| `T_SysCursor`（`+0xaa0e4`，ctor `sub_4B88C0`） | 0 → **5** | **0x430**（`0x42c+4`） |

- 新类型 `T_Queue_CACHE_SYSTEMCURSOR`（6 条，`size` 0x1c；来自 `T_SysCursor+0x410` 的**就地构造**，vtable 直写 `0x4b88ff`）。
- `T_SE` 有 3 段 **10 元素数组**（`0x440`/`0x468`/`0x490`；`sub_4B5230` 走步循环 `0x4b5255 lea esi,[ebx+440h]` + `0x4b52a4 cmp edi,0Ah`）。
- 我另外收尾：`T_Command` 里落在 `T_Voice` 跨度内的重复成员（`0x14d0c..0x14d20`）与 patch 的 `T_Voice+0x46c..0x480`
  是同一批字节 ⇒ 去重（保留 `T_Voice` 侧），否则 `T_Voice.size=0x484` 会让 G1 判重叠（子代理已实测红过）。

## 任务 B 定案：**不是同一个类**（`sub_4BB810` 是布局适配器）

| 判据 | 证据 |
|---|---|
| **RTTI 基类数组**（新手段） | `??_R2SE@@8`(`0x53f40c`) 与 `??_R2Voice@@8`(`0x53f6ac`) 条目数都 **2**（自己 + `BaseObject`）；**校准** `??_R2Sound@@8`(`0x53f4a0`) 条目数 **3**（`Sound > SoundError > BaseObject`）⇒ 中间基类**会**出现在数组里 |
| 构造器调用 | 两者各自唯一直接调用都是 `sub_4034A0`（`0x4b4e89`/`0x4bb729`）= BaseObject ctor，只写 `0x0`@`0x4034a2` 与 `0x4`@`0x4034a8` ⇒ 覆盖不到 `+0x408` |
| 决定性 | `sub_4BB810` 的 4 个调用点里，**同一个调用方**分别传 SE 槽与 Voice 槽：`sub_40A8A0 @0x40a900(+0x143bc)/@0x40a939(+0x148a0)`、`sub_417800 @0x417cb9/@0x417cf2` |

⇒ 载体 **c125 `sub_4BB810 → T_408_trio*`**（新类型，`adapter:true`，3 个 dword，**不写 `size`/`complete`、不猜基类**）。
★ 反事实：把它硬记成 `T_SE` 也能让冲突变 0 —— 那是**假绿**，未采纳。
★ 传播：**冲突 1 → 0**（可推导函数 417 不变；"声明 > 推断"的 note 181 处 / 6 个被调方）。

## 实测

16 个载体 / 115 站点：解释 **14（baseline）→ 101（强档）→ 109（强+弱）**。前 5：
`sub_4BB840` 0→18/19 · `sub_4BB700` 0→17/17 · `sub_4B4E60` 0→15/15 · `sub_4B88C0` 0→11/11 · `sub_4BBAB0` 0→4/5。

## 诚实项

- **`T_Voice.size = 0x484` 是我的判断**（lo = 构造器写入上界）；hi `0x490` 是下一个子对象的 this 槽，**中间 12B 零观察**。
- 弱档 **7 条**（`T_Voice 0x8/0x424/0x428/0x42c/0x454/0x458/0x484`，中 2 弱 5）**没有合并**（合并会假绿）。
- `T_SE` 的 3 段数组构造器**没写**，但 off/count/stride 逐字可读（循环边界）—— 若按"只有 ctor 写过算强"的严格口径，应移弱档。
- `T_SE+0x408`、`T_Queue_CACHE_SYSTEMCURSOR+0x4` 进 **G7 空话声明**（声明 ptr 但模型内零解引用），⛔ 不算通过；G7 现 **56** 条。
- 施工期间 `data/shape` 被并行批次并发写入 ⇒ 该批把基线**冻结**成 `_input/` 快照（sha256 在 MANIFEST），
  `build.mjs` 只读快照；合并时我按**当时仓库**重跑 claims / propagate / calls。

## 复现

`pnpm tools shape claims data/shape/carriers.json`（守恒① 5459 = 5093 + 366）· `pnpm tools shape propagate`（417 · **冲突 0**）·
`pnpm tools shape calls`（1662 对 · 0 不一致 · 43 处基类相容）· `pnpm test`（398）·
`node .tmp/shape/batch-10/se-voice-cursor/build.mjs --check`。
