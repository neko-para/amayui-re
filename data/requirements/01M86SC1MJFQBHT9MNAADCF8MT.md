# SMC 取证：AOT 运行期是否写可执行页（写点清单 + loud 停机的风险）

- id: REQ-01M86SC1MJFQBHT9MNAADCF8MT
- type: spike
- status: done
- parent: REQ-01M7FRY04PTP9788QZPVPKA5ND
- done_reason: 静态普查完成：.text 395,092 条指令 0 条写可执行页；poly = 289 条 IAT thunk（可达、只读）；无 PAGE_EXECUTE 分配、无绝对写 IAT。动态通道（rep movs/memcpy/shim）需页保护 + VEH report 才能定量，已写为重开条件；ON+watch 口径不可用（watch 是 LIFT_WATCH spec，不是写代码页）。
- tags: [emulator-lift, aot, smc, spike]

## 结论（2026-11-24）

**不可判（静态证据强烈偏向「不会停」）**：pin 的 PE 是 `corpus/assets/age/AGE.EXE__dumped.sectfix.EXE`
（sha `1937F086…B134`，与 `dist/install/AGE_DUMP.EXE` 同 sha）。整份 `.text` 的 **395,092** 条指令里
**0 条**写可执行页（`.text` 0x401000..0x51D000 / `poly` 0x5C3000..0x5C4000）；`rep movs` / memcpy 内建 /
shim 写这类基址动态通道静态测不到，要定量只能做一次页保护 + VEH 的 **report 档**运行。

- **`poly` 不是多态段**：`poly`（VA 0x1C3000 / VSize 0x1000 / char `0xE0000060`）是 **289 条
  `jmpl *0x51d0xx` 导入 thunk**（目标全在 `.data` 的 IAT，69 个不同槽）。`.text` 里 `calll 0x5c3…` =
  289；`closure.txt` 有 49 个 poly 根（全 `call-target`）；`lift_table.inc` / IR 有 289 个
  `lift_cc_5C3xxx`（全抬升）；`aot_host.cpp` 记录启动链在 `poly:005C3627` 停过 ⇒ **可达且被执行**，
  但**只被读、没有任何写点**。
- **静态写点 = 0**：`llvm-objdump -d --no-show-raw-insn --start-address=0x401000 --stop-address=0x51D000`
  解析 395,092 条；2,083 条绝对目的操作数 = 1,609 条 TLS cookie + 464 条写 0x55xxxx 数据区 +
  10 条字节流错位伪指令 + **0** exec。独立正则复核 `, 0x5[cC]3xxx$` = 0、`, 0x50xxxx$` / `, 0x51[0-3]xxx$` = 0。
- 相关但非 SMC：`VirtualProtect` 仅 1 个直接调用点（0x5009E6，`PAGE_READONLY`）；`VirtualAlloc` 5 处
  全是 `PAGE_READWRITE`、**无 `PAGE_EXECUTE`**；绝对写 IAT 槽 0 条。

## 关键纠正：`ON + watch` 口径不可用

`watchOn()` = `g_watch_count != 0`（有没有 `LIFT_WATCH` spec，`memory.h:144`），**不是**"写代码页"；
`LIFT_WATCH` 只能 ≤8 段 × ≤16 B，覆盖不了 `.text`；而 `LIFT_AOT_NOTE_WRITE=ON` 只给 lifted store
一条通道打点，`rep movs/stos`、memcpy 内建、shim 写都走 `noteGuestWrite(cpu.cache=nullptr)`
⇒ AOT 上 no-op，会得到**假的 0**。⇒ 要定量只能走实验 B（装载后 `PAGE_EXECUTE_READ` + VEH report 档）。

## 残余风险与重开条件

- 残余：基址动态写（`rep movs/stos`、memcpy 内建、shim 裸写）没有静态覆盖；若其中一条指向代码页，
  AOT 臂会静默跑旧 IR（`REQ-01M7FRY04PTP9788QZPVPKA5ND` 的 SMC 缺口）。
- 重开条件：① 出现 guest 自改代码导致的执行不一致（缺陷单）；② 静态/动态发现真正的写点；
  ③ 要跑一个已知自解压/自改的目标。重开时用实验 B（页保护 + VEH report），⛔ 不要用 `LIFT_WATCH`
  或 `LIFT_AOT_NOTE_WRITE=ON` 当"写代码页"的证据。
- 旁证产物：`.tmp/reveq/REQ-01M86SC1MJFQBHT9MNAADCF8MT/`（`_text.disasm.txt`、`_abs_dest_stores.tsv`、
  `_string_ops.tsv`、`_iat_map.tsv`、`scan_stores.ps1`、`parse_iat.ps1`）。
