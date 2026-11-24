# M-lift-2 · 真翻译：解释回退归零 + 去掉逐指令宿主调用 + 寄存器跨块 SSA + call/ret 真调用（release 端到端为唯一速度判据）

- id: REQ-01M7FC3W0ZMA35PK3WV73YXB1Q
- type: req
- status: done
- parent: REQ-01M7JHSEZVMYB4XHEN32V7V6HJ
- done_reason: 判据 1 达成（lift_one IR 调用点 0；tick/note 的 native 调用点 0，IR 残留 74,691 个 tick call 由 LTO 内联）；判据 2 剩余拆到 S7/派发密度子单、判据 3 降级 M-lift-4（alloca 判负，SSA 建议 drop）、判据 5 为 2026-11-15/16 历史读数。读数口径 2026-11-24 已核，无新增守卫（shape 依赖，§10 不进 test）。
- tags: [emulator-lift, lift, efficiency]

## 结论（2026-11-24 收口）

判据 1 的「解释回退归零」达成：`lift_one` 的 IR 调用点 = **0**（产物里只在注释出现），运行期没有入口。
其余按 2026-11-15 的收口口径处理：判据 2 的剩余覆盖率已拆到子单、判据 3 降级到 M-lift-4、
判据 5 是 2026-11-15/16 的历史读数。

## 判据（2026-11-24 定稿）

1. **解释回退归零**：`call … @lift_one` 在 8 份 `lift_aot.part*.ll` 里 = **0**；`_lift_tick` /
   `_lift_note_write` 的 **native** 调用点 = 0。⚠ 口径：IR 里仍有 **74,691** 个 `call void @lift_tick`
   （LTO 在链接期内联成 58,419 个 `addq …,0x6e0`）；`@lift_note_write` 的 IR 0 是
   `LIFT_AOT_NOTE_WRITE=OFF` 关出来的，不是内联 —— 这两条归 M-lift-4 记账。⛔ 不再要求 IR 里 tick = 0。
2. **`call` / `ret` 真宿主调用**：S7 判据 4 的放宽已收（`REQ-01M81PTTC7XBK365YGQNQTTXWA`：可结构化
   5,343→6,411、宿主 `call` 站点 1,572→2,967）；剩余覆盖率不进本节点，见
   `REQ-01M86NMT0YJ9V78XS2DZZV364Q`（S7 覆盖面）与 `REQ-01M86NNG4YMF68NCAX0NT45Y22`（派发密度）。
3. **guest 寄存器跨块 SSA**：降级到 M-lift-4；该笔已由 `docs/03-emulator/aot-alloca-evaluation.md`
   判负（Cut A `__text` −0.84%、spill +151%；Cut B 未测、估计 <2%），2026-11-24 复核建议 drop。
4. **EFLAGS 块内惰性**：守卫 `ctest:lift-ir-proto`（`tests/lift_ir_proto.cpp` 的 S3-c 三条）；产物头
   块 74,502 / 零 flags 块 40,074 / 屏障（`lift_one`）站点 0。
5. **release 端到端**：2026-11-15 300 帧中位 **8.14 s**（门槛 15 s）；3000 帧五项指纹全过
   （`exit 0` / `frames 3000` / `stop @ 00498AE4` / `misses 0` / arena `108595152·108595152·21`）；
   2026-11-16 交错 A/B 300 帧 4.97 / 4.99 s。⛔ 都是历史读数，不是今天 HEAD 的值。

## 读数（2026-11-24 / win32 / dev / `split8-O2-lto-abi`）

- `rg -c --glob 'lift_aot.part*.ll' 'call [^;]*@lift_one' out/aot` ⇒ 0。
- 同上 `@lift_tick` ⇒ 74,691（8 份合计）；native 端 58,419 个内联累加，独立 `_lift_tick` 函数体 0。
- `@lift_note_write` ⇒ 0 个 call、0 个 declare（默认档 OFF，见 M-lift-4）。
- 派发 / 运行期读数见 `docs/03-emulator/aot-s7-evaluation.md` §4.4 与 `aot-dispatch-density.md` §2。

## 为什么单独立（历史）

`M-lift-1` 证的是能力（`entry → TITLE`、派发面 `handoffs == 0`），没证「这是翻译而不是解释」：
`OBS-20261115-04` 的 300 帧里 47.1M 条指令走 `lift_one`（占墙钟 85%）；`lift_tick` / `lift_note_write`
是另两类逐指令宿主调用。本节点把「生成的代码本身变成真翻译」作为目标。⛔ 执行源只有 IR 与 shim
（§12.8），不为某个 EA 开特判。

## 边界

- ⛔ 不按 EA 特判；⛔ 不改 `insns` / 时钟 / 定时器语义；⛔ 不为达标删 CTest。
- x87 精度见 `docs/00-origin/decisions.md` §8.1（统一 double；PE 恒 0）。
- 已裁决不再投入的两处（tick/note 的 IR 内联、跨块 SSA）由 M-lift-4 记录取舍。

## 收口

本节点 done 的凭据是**判据 1 的 0**（能力存在、可复核）+ 2026-11-15 的收口裁决（用户已定）；
判据 2/3 的剩余不在本节点（子单 / 欠账单），端到端下一量级在 M-lift-3（图形 shim）。
