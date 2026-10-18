# 批 S2t · 掩码表裁决（Command+0xa30d8 → T_el32×1024）+ G4 第三档（末尾==size）

- id: REQ-01M56QY799GA5HVY5C9VS4EYVM
- type: req
- status: done
- parent: REQ-01M54KEF7X560CMPMGABG2W3CW
- done_reason: Command+0xa30d8 数组化（memset 0x1000 + 256轮×4dword 全读 + 三连掩码用法 ⇒ 非指针）+ Command+0xa40d8 补标量 + G4 第三档；守卫 ★㉔ 与两条变异；claims 残差 534→500
- tags: [engine, shape]

# S2t · 掩码表裁决（`Command+0xa30d8`）+ G4 第三档（数组正好填满对象）

## ① `Command+0xa30d8`：183 条离散成员 → `T_el32 × 1024`

原来的挂起理由是"元素语义没定（构造器往 `0xa3100+` 写的是 `&table` 一类的值）"。**读代码后这个理由不成立**：

- `0x417341 push 1000h` + `0x417346 lea eax,[esi+0A30D8h]` + `0x41735F call _memset`
  ⇒ 整段 **0x1000 字节**（= 1024 个 dword）被清零；
- `0x41795A lea ecx,[esi+0A30D8h]` + `0x417960 mov ebx,100h` + `0x4179CC add ecx,10h` / `0x4179D0 jnz`
  ⇒ 256 轮 × 每轮读 4 个 dword ⇒ **1024 个 dword 全被读**；
- 读点是 `mov eax,[ecx]` → `test`/`jle` → `and edx,[ebp+hFile]` → `cmp` 三连，**从不被解引用**
  ⇒ 它是**"所需能力位"掩码**（配不上就把同下标的 handler 重置成 `sub_418E30`），不是指针；
- 构造器写的是 `0x417364 mov eax,1` 之后的连续 `mov [esi+0A310xh],eax` 与 `mov […],ebx`（0）——
  `lea eax,[esi+0A30D8h]` 的结果**只被 `push` 进 `memset`**，不是存进表里。

⇒ `{off:"0xa30d8", type:"T_el32", count:1024}`（`T_Command` 成员 422 → 240）。

## ② 顺手补上紧随的 `Command+0xa40d8`

`0x4142A6 mov eax,[esi+0A40D8h]` + `neg`/`sbb`/`and 0FFFFFFFEh`/`add 2` = 布尔转 1/2，**从不被解引用** ⇒
独立标量 `{off:"0xa40d8", width:4}`（原来漏声明 ⇒ 32 处站点一直在残差里）。

## ③ G4 第三档：`end === size(容器)`

见 S2m。要点：贴边是可证伪的**等式**（不是不等式），且越界由 G1 兜住。

## 效果

`claims`（全模型）守恒①：**6498 = 解释 5964 + 残差 534** ⇒ **6498 = 解释 5998 + 残差 500**（−34）；
其余守恒与义务不变；`findings` 仍为空。

## 守卫

- 用例：`tools/test/shape-claims-guards.test.mjs#★㉔ G4 第三档：数组**正好填满对象**（末尾 == size）是边界证据；把 count 改大一格就必须红`
- 变异：`G4 第三档被拆掉` / `第三档的等式被放宽成"末尾 ≤ size"`（`tools/mutate-check.mjs`）
- 口径：`docs/02-engine/shape-census.md`「第三档：数组正好填满对象」
