# 前沿取证 · sub_41BF50（被 41 个已登记函数调用）

- id: REQ-01M4G9XYSK4DC3C49YJ8KVBNZS
- type: req
- status: done
- parent: REQ-01M4G9XYQG149FB3VG5Y6FW3YY
- done_reason: 台账已落 subject `Engine+0x41BF50/read-primitive-14-case-table`（12 个 EA 锚：函数起点/跳转表/六个 case 落点/文本原语的互补表）：逐 14 case 的池偏移 + 是否 DEC + 指针族"解引用后再 DEC"全表给全。两个副产物：① 独立复核了 LOCAL_POOL_SLOTS 的六格（case 9..13 = 帧+0x34/38/3C/40/44）——这是该常量的第 3 条独立证据；② 发现读值路缺席 type 8/0xE（都抛），与文本原语 sub_41B640 的表互补。它的出边没有前沿（不调任何 sub_*），价值在入边（392 个调用方 / 1200 处调用）。
- tags: [engine, coverage]

## 为什么是它

`pnpm tools ledger coverage` 的前沿**第一名**：被 **41 个"已登记"函数**直接调用（取值原语 = 读操作数那一层），
而台账里**没有任何 `bin` 锚落在它身上**。它是"所有人都依赖、却没人登记过"的那个洞。

## 已知（来自台账，锚在别处）

* 台账 `Engine+0x42AEA0|0x41BF50|0x42B4B0/operand-type-switches`（accepted）说：
  **"操作数类型分派 = 三套不同的支持集"** —— 读值 `sub_41BF50` → `jpt_41BF99`（**14 项**），
  入口 `cmp edx,0Dh` + `ja def_41BF99`，且 **case 8 走 default（= 抛 `Command_Type_Exception`）**。
* ★ 那条记录**只锚了一条守卫用例**（`tools/test/engine-operand-types.test.mjs`），没有 EA 锚 —— 所以覆盖度查询看不见它。

## 要产出的（收口判据）

1. **逐 case 的表**：14 项跳转表里每个 case = 哪种 operand type / 读哪个池 / 取格后是否 DEC /
   是否解引用后再 DEC（`pools.ts` 的注释里已有 case 9 与 case 12 的逐字，但那是**两格**，不是全表）；
2. 一条台账观察，锚 = 函数起点 EA + 跳转表地址 + 各分支代表性 EA；
3. 守卫草案：**能机械判**的（例：case N 的池基址 == `LOCAL_POOL_SLOTS` 里 typeTag N 的 base；
   或"支持集 case 数 == 14 且 default 落在 case 8"）；
4. 它 callee 里**新暴露**的未登记函数 ⇒ 登记进后续节点。
