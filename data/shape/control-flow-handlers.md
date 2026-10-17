# `control-flow-handlers.json` —— **指令跳转 / 调用族** handler 的载体表

> 同名数据文件的说明书。字段 / 义务 / 守卫的真源是 `pnpm tools shape describe`（本文件不复述 schema）。
> 本文件**只放载体**（机器槽 → 类型）；类型在中心真源 `types.json` 里，走 `typesFrom` 引用。

## 它声明什么

"这一族的 handler 以 `ecx` 为对象根（`obj0`）"——每条载体 = `{ id, kind:"arg", fn:<handler 符号>, reg:"ecx", type:"T0*" }`。
判定器把逐 site 访问投到 `types.json` 的成员表上，给出解释 / 残差，并检查 G1..G8、J3 与守恒①②③。

**覆盖范围（判据，不是名录）**：`call-script` · `ret` · `exit` · `jmp` · `call` · `local-ret` ·
`load-frame` · `call-frame` · `exit-script` · 以及与它们同族、共用同一片 Engine 级的几个无名 handler。
opcode ↔ handler 符号现查：`pnpm tools opcodes handlers`（⛔ 映射是"桥"，不写进本 JSON）。
★ **`abort`（`opcode 0x0001`）在本族里但故意没有载体**：它的 `.c` 约定里**查不到对象根寄存器** ⇒
建了载体也**无法检查**（判定器会红）。"不可检查"要如实报，不许拿一条空载体冒充覆盖。

## 这一族给中心类型表补了什么（**为什么单独立族**）

* **帧记录（`T1`，`0x78` 字节）的字段**基本由这一族补出：它碰帧内的多个元素内偏移，
  而数值族只碰**一个**（`0x60`）。⇒ "记录里有哪些槽"这个问题，数值族答不了。
* **Engine（`T0`）的字段**：这一族碰大量 Engine 级绝对偏移（含每帧数组、二级对象指针）。
* ⇒ 推论：**族不是按 opcode 段切出来的**，是按"它碰了对象的哪一片"切出来的；
  全表口径（`pnpm tools shape census`）才是族边界的依据。

## 非显然口径（这一族暴露出来的）

* **同一条字节、两套原点**：`0x5d8d0` 既被"无索引的绝对位移"碰（`0x41abd4`），
  也被"记录基址 + `0x78`·索引"碰（`0x41abf9`，元素内 `0x3c`）。本模型**不**把 `0x5d8d0` 声明成 `T0` 成员
  （那会与数组窗口重叠 ⇒ G1 红），而是让它经**数组 → 元素内 `0x3c`** 被解释 —— 这正是
  `docs/02-engine/shape-census.md` §4"原点约定不许静默选一个"的落点。
* **`kind:"ptr"` + `type: null` 的两条**（Engine 级指针成员）：它们的二级访问进**不透明桶**
  （守恒②里"不透明"那一档），⛔ **不算通过** —— 目标类型给不出来，就是给不出来。
* **每帧数组不能声明成数组**：`0x5ee04` 的访问是 `0x5ee04 + 4·索引`（索引 = Engine 的一个标量），
  但**元素条数没有依据**（数组末尾地址在语料里不成立）⇒ 只能声明成**内联 dword**；
  另一处二维形态（`0x5eea4`，索引身份解不出）连内联都表达不了 ⇒ 进**二级对象内嵌派生 / 未解析**桶。
  这类"形态可分、边界不可分"的取舍由 `REQ-01M530BCT4ACNMY9ZXJCYR02MT` 裁决。
* **负原点**（真内存访问里索引是 `值−1`）与 **`lea` 取址中间量**（`abs` 只是位移项）**不是一回事**：
  前者 `unverifiable`、后者进"位移不可用"。判据函数 `offsetUnusable()`；⛔ 按 `abs < 某值` 一把抓会误报。
* **复核器与助手不一致的据点**（两侧独立实现，解开的不一样）：不一致处**不许当"已解释"** ——
  它们是"复核覆盖率是表的函数"这一条诚实项的具体来源（见 `docs/02-engine/shape-census.md` §9）。

## 怎么查

```bash
pnpm tools shape describe                                  # 义务 / 守卫 / 诚实项（先看这个）
pnpm tools shape claims data/shape/control-flow-handlers.json   # 本族：解释 / 残差 / 守恒①②③（退出码 0 = 绿）
pnpm tools shape claims data/shape/types.json              # 类型真源自校验（本族的成员都进那里）
pnpm tools shape access sub_41A9B0                         # 逐 site 访问记录（EA 锚）
pnpm tools shape verify 0x41aa0e                           # ★ 独立复核器：按 EA 回语料重解（不信转述）
pnpm tools shape census                                    # 全表普查（族边界 / 偏移聚类 / 未解析报数）
```

## 怎么改

1. **加载体**：先 `pnpm tools shape access <handler>` 拿观察，确认对象根寄存器与 `type`，再加一条**中性编号**的载体；
   若在 `.c` 约定里查不到对象根 ⇒ **别加**（那是"不可检查"，如实报）。
2. **加成员 ⇒ 改的是 `types.json`**（类型是中心真数据）。⛔ 不要在本文件里另立一份类型；
   同一个 `off` 若与已有成员冲突（宽度 / `kind` 不同）⇒ **先判哪个对**，别让"先写进去的那个"赢
   （踩过：提案里同一个 `off` 出现两次、声明不同，naive 合并会静默留下弱的那条）。
3. **改完跑** `claims data/shape/types.json` 与**每一个**族文件（类型共享 ⇒ 一处改动影响所有族）。
4. **判不了就报残差 / 异常**：本族的残差主要来自未解析基址与 `exit-script` 那一带的散点，
   它们**不是**失败，而是"下一轮要做什么"的清单。
