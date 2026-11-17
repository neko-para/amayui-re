# 控制层读数缺口：读脚本局部/全局槽（解码后）——SC0000 三个卡点都缺它

- id: REQ-01M7M7C7JFYHQ587TP75HVQZQN
- type: req
- status: open
- parent: REQ-01M7HC1WJ9X0QCQ51Q9F01988W
- tags: [emulator-lift, control, sc0000]

## 要做什么

控制层的 `sample` 现在只有 `script` / `pos` / `frame` / `insns` / `screen`，**读不到脚本局部变量的值**。
SC0000 的三个卡点都卡在这上面：

- `SBUNKI` 的确认 gate 用 `(local 9)` / `(global f8dfc)` / `(global f8dfe)`；
- 命中测试 `i12e` 的 out 是 `(local 7)`，`local 9 <- local 7` 在下一拍（`0x1124`）；
- `(global f8fa5)` 的位（`&4`）与存盘设置 `(global a9cd) & 1`。

⇒ 加一个**只读**读数：按（槽类, 槽号）读当前帧的脚本局部/全局变量，返回**解码后**的整数值。

## 判据（不许猜）

1. **同一份解码**：int 族槽必须过引擎自己的 DEC（`Engine+0x5EC8C` 的 key，`ROR25(ROL11(v)^K)`，
   台账 `KN-…`/`tool` 里已有），⛔ 不许另写一份"看起来像"的解码；key 从 guest 读，⛔ 不设默认值。
2. **同一份几何**：帧基址 `Engine+0x5D880`、步长 `0x78`、`cur = Engine+0x5D880`、局部池基址
   `帧+0x34..+0x48`（台账 `Engine+0x5D880/frame-fields`）；跨出 `0x78` 或槽号越界 ⇒ 响亮错，⛔ 不夹取。
3. **只读**：不写 guest 内存、不推进时间、不改变任何现有读数；暂停态可读（`sample` 同一把锁）。
4. **协议**：`emu-ctl` 新 op（如 `read-slot`）或 `sample` 的扩展字段；`--describe` 必须写清
   "值已解码 / 槽类 / 越界行为"；⛔ 不让调用方自己拼地址。
5. 守卫：顶层 pure CTest 或 `tools/test/` 的契约用例（自造 guest 内存 + 已知 key 的往返），
   ⛔ 不断言当前运行时的具体数值。

## 为什么现在需要它

SC0000 的加载确认框（`REQ-01M7M45WSPSYB6XC82TS3X4YM2`）已经排到"只差读一个槽"：
`(local 9)`/`f8dfe`/`f8dfc` 的值一到手，就能判"命中测试没中 / 命中但 gate 的另一半不成立"。
没有它就只能继续盲试点击（已经烧掉 8 个 run）。

## 边界

⛔ 不做写槽 / 改槽；⛔ 不做"通用内存读"（那会绕过槽几何与解码，等于给 agent 一把没有语义的枪）。
