# emulator-new：补**指针池**并把 `0x61 lookup-array`（及其 2d/64 兄弟）落地

- id: REQ-01M5F8JE55PWCDAJM941GFV7FV
- type: req
- status: done
- parent: REQ-01M3TCB0S0BTW41KW0JBJMWHHH
- verify: tools/test/emulator-new-pointer.test.mjs#★★ `0x61`
- tags: [emulator, frame, pools]

## 一句话

启动链原来停在 `0x61 lookup-array`（第 140 条）。它挡住链的原因不是"少一个 handler"，而是**没有指针池与地址**：`0x61` 的落点是引用/指针族，算的是**地址算术**。本单把这层补上，链推进到第 **14212** 条（`0x2de`，另一个主题）。

## 做了什么

1. **合成地址空间**（`apps/emulator-new/src/address.ts`）：扁平 / 合成 / 稀疏——
   全局池 `0x10000000` 起每池 64 MiB（顺序 3/4/5/6/7/8）、局部池 `0x40000000` 起每帧 8 MiB（帧内六池各 1 MiB）；
   元素宽度：字符串记录 28、其余 4；越界 / 未对齐 / 落在窗口外 ⇒ 响亮失败。
2. **指针池**：`Frame` 加 `localPtr`(12) / `localFloatPtr`(13) / `localStringPtr`(14)；
   `Machine` 加 `globalIntRef`(6) / `globalFloatRef`(7) / `globalStringRef`(8)。指针槽**不编解码**。
3. **原语**：`addressOfOperand`（取址，值族给槽地址、指针族给**槽内容**）、`readInt`/`readFloat`/`writeInt`/`writeFloat`
   对 6/7/12/13 **解引用**后读写；`writePointerSlot`（写指针槽）。
4. **`0x61 lookup-array`**：`dest ← 取址(op2) + 元素宽度×读整(op3)`；步长按落点族定（6/7/12/13 ⇒ 4，8/14 ⇒ **28**），
   值族落点 ⇒ 抛类型异常（`sub_418CC0` 的 default 分支）。**顺便**把同写入器的 `0x12c lookup-array-2d`（`行×行宽+列`）落地。
5. **配置键**扩展（`55-config.md`）：引用/指针族的键下标 = `(地址 − 该族全局池起点) / 元素宽度`，
   值族仍用池下标 ⇒ `INITCONFIG4` 里 `save-int [type 12]` 不再抛。
6. **`0x64 copy-local-array`**（链上紧随其后的缺口）：脚本体里一块整数（首 u32 = 个数）逐格 `ENC` 写到 op1 的地址；
   块偏移 = `body 4×raw`（body 已切头），空块 ⇒ 不写。
7. **守卫**：`tools/test/emulator-new-pointer.test.mjs` 9 条（步长 28 / 地址算术 / 落点族 / 解引用 / 未映射未对齐 / 配置键换算 / 0x64 / 0x12c）；
   **变异 5 条**（步长 4 / 漏乘元素宽度 / 解引用用槽下标 / 键按下标算 / 块偏移漏 4×），各自 `--match` **1/1 红**。

## 效果

启动链棘轮：**140 → 14212** 条；缺口从 `0x61`（INITCONFIG4）推进到 **`0x2de`**（CHECKCONFIG 偏移 0，字体名查表 —— 需要宿主字体表，另开单）。

## 诚实边界

- 地址是**合成**的（不是引擎真实地址）：只保证同一批输入给出同一批地址、且算术自洽；真实地址不是可观察语义。
- 字符串池的**局部**槽（type 11）仍未建模：字符串指针指向它时，`set-string` 的落点分派会响亮失败。
- `0x64` 的源块按引擎逐字**无条件编码**；目标族是浮点时写的是编码后的位模式（与引擎"写 4 字节"一致）。
