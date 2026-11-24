# M-lift-2 · 真翻译：解释回退归零 + 去掉逐指令宿主调用 + 寄存器跨块 SSA + call/ret 真调用（release 端到端为唯一速度判据）

- id: REQ-01M7FC3W0ZMA35PK3WV73YXB1Q
- type: req
- status: doing
- parent: REQ-01M7JHSEZVMYB4XHEN32V7V6HJ
- tags: [emulator-lift, lift, efficiency]

# M-lift-2 · 真翻译：解释回退归零 + 去掉逐指令宿主调用 + 寄存器跨块 SSA + call/ret 真调用（release 端到端为唯一速度判据）

- id: REQ-01M7FC3W0ZMA35PK3WV73YXB1Q
- type: req
- status: doing
- parent: REQ-01M7JHSEZVMYB4XHEN32V7V6HJ
- tags: [emulator-lift, lift, efficiency]

## 为什么单独立这一节点

`M-lift-1`（`REQ-01M7CZH25AVZWVZH4JH408CJPB`）已 done，它证的是**能力**：`entry → TITLE`、
派发面 `handoffs == 0`（没有把 EA 交回解释器的路径）。但它**没有**证"这是翻译而不是解释"：

- 判据 1 的 `handoffs == 0` 是在**派发层**数的（`misses=0` / 117M 次查找）；
- 实测（`OBS-20261115-04`）：同一轮 300 帧里 **47.1M 条指令走 `lift_one`**（块内的单指令解释回退，
  它内部就是 `Decoder` + `execInsn`，即解释器本体）——占指令数 **5.1%**、占墙钟 **85%**。
- 逐指令宿主调用还有两类：`lift_tick`（125,860 个调用点）与 `lift_note_write`（37,093 个，且它在 AOT 臂里是 **no-op**）。

⇒ 本节点把"**自动生成的代码本身**变成真翻译"作为唯一目标。⛔ 判据里不接受**第三份语义实现**：
执行源只有 IR 与 shim（§12.8 第 1 条）。
⚠ 2026-11-17 订正：本行原写"`--native-overrides` **缺省空档**下的读数才是判据" —— 该参数在 AOT 臂已是
**未知参数**（`exit 2`，§12.5），且 `repo:tools/test/aot-arm-shape.test.mjs` 把它列为禁用词 ⇒ "缺省空档"没有
第二种取值，那句话是空转。读数口径改为**只有缺省档一种**（没有可选的 override 档）。

## 判据

1. **解释回退归零**：生成的 `.ll` 里 `call … @lift_one` 调用点 = **0**；`@lift_tick` = **0**；
   `@lift_note_write` = **0**。任何残留调用点必须逐点给出机械理由，且**运行期计数为 0**（不是"很少"）。
   ⛔ 不许用"把 `-O2` 编出来看不出"当达标依据。
2. **`call` / `ret` 走真宿主调用**：出分量的 `call`/`ret`（基线 `escape` 12,749 处：`ret` 7,002 / 间接 5,716）
   改为宿主直接调用（S7 的判据与取舍见 `apps/emulator-lift/src/aot/lift_struct.h`），
   `dispatch total` 相对基线下降（给读数）。
3. **guest 寄存器跨块 SSA**：同一个 `lift_cc_*` 内寄存器不再每块刷回 `%st`；`objdump -d` 的机器指令行数
   相对基线下降（给读数），且 `.text`/guest 字节比向盲写对照的 **335%** 靠拢（基线 **810%**）。
4. **EFLAGS 仍是块内惰性 + 机械分析**（D41 / D45 的口径），⛔ 不回退成"每次物化六位"。
5. **release 端到端**（唯一速度判据，窗口臂 `--skip-logo --present window`）：
   - 300 帧墙钟中位 **≤ 15 s**（基线 84.2 s，写这一条时的读数）；
   - 3000 帧验收臂 `exit 0` / `frames 3000` / `misses 0` / `stop @ 00498AE4` / arena `108595152·108595152·21`
     **逐位不变**（`OBS-20261115-03` 的那一套）；
   - ⛔ 速度只在 release 构建、同一台机器、交错 A/B 下取中位数；⛔ 不用 Debug 的读数。

## 收口口径（2026-11-15 裁决，用户已定）

实测（`OBS-20261115-06`）：**翻译侧已达 native 量级**（929M 条 guest 指令 / 1.6 s = **1.7 ns/条**；
派发只剩 ~0.3 s），而端到端墙钟的 **78% 已在图形 shim**。⇒ 判据按**目的**收口，不按字面收口：

- **判据 1 照做**（`lift_one` 归零）—— 它既是字面判据，也是剩余非图形开销里最大的一块（≈0.76 s / 9%）；
- **判据 2 已在 S7 落一部分**（`dispatch` −37.6%、`ret` 站点转 25.4%），**剩余覆盖率不再投入**：
  收益已进入这台机器 5% 的噪声（`dispatch` 只剩 ~0.3 s）；
- **判据 3 与判据 1 的 `lift_tick` / `lift_note_write` 部分降级为欠账**，单独立单
  `REQ-01M7FRY04PTP9788QZPVPKA5ND`（`M-lift-4`），理由是它们合计只剩 **1–2% 墙钟**；
  ⛔ 该欠账单不许被当作本节点的前置（父子关系已故意做成**兄弟**）。
- **判据 5 已达成**（300 帧 8.14 s，远低于 15 s 的门槛）。

⇒ 本节点以"**翻译侧达 native 量级 + 解释回退归零**"为 done 的口径；端到端的下一个量级在
`REQ-01M7FRXZ3K23M0WVTJQ9NNN35B`（`M-lift-3`，图形 shim），**不属于本节点**。

## 不做 / 边界

- ⛔ 不新增"因为某块慢就按 EA 特判"的路径。
- ⛔ 不把 `insns` / 时钟 / 定时器语义改掉（它们参与 `--max-insns` / `--time-limit` / 停机诊断）。
- ⛔ 不为达标去装 override、不为达标删既有 CTest 证据。
- x87 的精度裁决见 `docs/00-origin/decisions.md` §8.1（**统一用 `double`，不追 80 位；PE 恒 0**）。

## 进展（2026-11-24）

- **判据 2（`call`/`ret` 真调用）在兑现**：判据 4 的可证放宽已收（`REQ-01M81PTTC7XBK365YGQNQTTXWA`：可结构化 5,343→6,411、宿主 `call` 站点 1,572→2,967）；同分量边直连 + 跨分量 `jcc` tail call 已收（`REQ-01M85TQW3Z2QR2KWGCQB34KWAP`：静态同分量 `ret i32` 1,897→0）。
- **运行期**：`iter` 64.07→51.07 M、`insns/iter` 16.50→23.64、点击大帧 409→210 ms（`repo:docs/03-emulator/aot-dispatch-density.md`）。
- **下一层已拆单**：派发密度/块合并 `REQ-01M86NNG4YMF68NCAX0NT45Y22`；不可跟踪帧恢复的间接 `call`（热泵 `004BBAB0` / 主循环 `00412290`）`REQ-01M86NMT0YJ9V78XS2DZZV364Q`。
- **判据 5 的绝对值口径**：8.14 s / 15 s 是 2026-11-15 的读数，⛔ 不要拿它当今天的判据（本机分辨力见 `AGENTS.md` §12.6）。
