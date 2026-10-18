# 批 S1 · 对象 shape 普查与独立复核（一次性产物 + 复核器）：槽 / 宽度 / 数组 / 步长 / 未知区间 + 函数间 this 链接

- id: REQ-01M52F3ASMGWQXX64JSA18MP2N
- type: req
- status: doing
- parent: REQ-01M3TCCVC01AWFEMP6GJM96KQ4
- order: 40
- tags: [engine, shape, tooling]

## 目标（shape，**不是**名字与语义）

把"这个对象在内存里长什么样"变成**可复核的数据**：有哪些槽、各多宽、哪里是数组、元素多大、步长多少、
索引从哪个槽来、哪些区间**未观察到**。名字 / 用途 / 类型身份**不在本单**（那走知识台账的准入门）。

## 口径（三条，决定架构）

1. **一次性工作，不要求机械可重现；但必须可独立复核** ⇒ 承重件是**复核器**，扫描器只是助手。
2. **复核器与助手必须是两个实现**（不共用折叠/解析代码），复核器**只读语料**、不读助手缓存；否则"独立验证"是自证的。
3. **宽度的闭集合来自 `defs.h`**（`_BYTE`=1 / `_WORD`=2 / `_DWORD`=4 / `_QWORD`=8 / `_UNKNOWN`=1）；
   推不出 ⇒ `unknown`，**不许当 4**。

## 范围（落点不是散文）

* 类型真源 **`data/shape/types.json`**（成员 = `off` + 宽度 / 类型+count / `count:null`+`sweep`）；
  载体（机器槽 → 类型）**只有** `data/shape/carriers.json`。⛔ 不许把类型写进族文件、也不许把字段写成载体。
* 链接三类必须分开记（同对象 / 内嵌子对象 / 成员指针），尾跳单独一类；跨函数合并**只允许**在有链接证据时做。
* 未解析项（base / index / 宽度）**必须报数**；"位移不可用"与"未解析基址"是**两个桶**，不许相加。

## 分期与收口凭据（★ 父单能不能关，只看这一格）

| 期 | 件 | 守卫 / 凭据 |
|---|---|---|
| S1a–S1f | 普查 · 两套原点 · 对象路径 · 跳转族字段 · A 层分档 · 条数未观察数组 | 各自的 `verify`（在各节点里） |
| S1g | obj0 覆盖欠账**三档**（`census --debt`：已声明 / `count:null` 窗口内 / **未声明**） | `shape-census-debt.test.mjs#★debt-01` |
| S1h | 值传播（栈槽存/取与栈槽常量；对象根经栈槽往返要找回来） | 节点里的守卫 |
| S1i | 段落活窗口（`noreturn` 之后的死代码 / SEH catch 块的过近似） | 节点里的守卫 |
| S1j | **闭合度 L0** + 未闭合按原因归档（`coverage --json`） | `shape-coverage.test.mjs#★arch-05` |
| S1k | 调用链类型对账（`calls` 三态 + G16 witness 落在调用点） | `shape-claims-guards.test.mjs#★⑭` |
| S2 | 全语料分批建模（载体单文件；每批六条验收） | ⬜ 逐批 `verify` |
| S1l | 跳表目标集导出（间接跳转 ⇒ 目标已知） | ⬜ |
| S1m | 值身份缺口（数组元素内的字段当基址） | ⬜ |
| S1n | 补载体后暴露的成员表缺口（7 个函数 ⇒ 6 修好 / 1 移交） | `shape-claims-g4-init.test.mjs#★g4init-01` |
| S1o | `calls` 覆盖扩展（栈传参 / 间接调用 / `local` 实参） | ⬜ |
| S1q | G2 第三档：整片 init 写 / 同一 dword 与其内字节字段 | ⬜ |

## 怎么跑（本单的读数全在命令里，⛔ 不在这里写数字）

```bash
pnpm tools shape census --debt                     # 覆盖率三档（"未声明"那一档才是欠账清单）
pnpm tools shape coverage --all --json             # 闭合度归档（按原因分组；"下一层能用哪几个函数"）
pnpm tools shape calls                             # 调用链三态对账（一致 / 不一致 / 单侧）
pnpm tools shape claims data/shape/carriers.json   # 判定器（G1..G9 · J3 · 守恒①~④）
pnpm tools shape propagate --json                  # 还有哪些函数"已被推断、尚无载体"
```

## 与既有工作的关系（不重复造）

* `packages/age-format/src/engine/layout.mts` 的**手写**槽普查（`FRAME_SLOTS_OBSERVED` / `FRAME_ENGINE_SCALARS` /
  `LOCAL_POOL_SLOTS`，守卫 `tools/test/engine-frame.test.mjs`）已被本单的普查产物**取代**；
  两处口径必须一致（改一处要连带看另一处）。
* ★ **两套原点必须显式**：`0x5D880 + 0x78*cur`（相对 cur 标量）与 `0x5D894 + 0x78*cur`（记录）是同一字节的两种写法
  ⇒ shape 表要记**原点约定**，不许静默选一个（先例：R1 的基址订正）。
* `disasm-fn` 的 `buildCatalog` 已把两侧扫描合成一遍 ⇒ 助手侧的 per-site 普查塞进同一遍，不新增第二个扫描器。

## 参照物：`defs.h` **已入库 + 已登记**（条目 `disasm/c-vocabulary`）

* 宽度闭集合与折叠内在函数以它为准；⛔ **不使用** `engine.hpp` 之类前人推断（未按准入门复核过）。
