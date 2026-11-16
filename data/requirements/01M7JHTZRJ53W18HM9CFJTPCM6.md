# 🐞S2 AOT 臂的 SMC 失效是静默缺口：lift_note_write 在 AOT 是 no-op ⇒ guest 写已发射代码页后仍执行旧 IR

- id: REQ-01M7JHTZRJ53W18HM9CFJTPCM6
- type: bug
- status: open
- parent: REQ-01M7JHSEKXW8KAKB6K2RS645M7
- repro: apps/emulator-lift/src/runtime/decode_cache.h#lift_note_write
- severity: S2
- tags: [emulator-lift, aot, smc]

## 现象

AOT 臂的 **SMC（自修改代码）失效是静默缺口**：生成的 IR 在每个 guest 数据写之前调 `lift_note_write(cpu, ea, n)`
（`src/aot/lift_gen_emit.cpp:1342` / `:2904`），而它的实现在 AOT 里**什么都不做** ——
`lift_note_write` → `lift::noteGuestWrite` → `cpu->cache` 的失效路径，而 AOT 的 `cpu_.cache == nullptr`（`aot_host.cpp:81`）。

⇒ guest 写自己**已发射**的代码页之后，AOT 继续执行**旧 IR**：不失效、不报错、不计数。

已由本仓自己写明（两处）：
- `src/runtime/decode_cache.h:8`：「AOT 臂的已知 SMC 缺口：`lift_note_write` 在 AOT 里是 no-op」；
- `src/aot/aot_host.cpp:913`：「（AOT 臂 `lift_note_write` 是 no-op，所以这条路径是唯一的防线）」。

解释器臂有真的失效路径（缺陷 `REQ-01M6JFZ7MQNG8JHA5XM26FRW88` 修过 `BlockCache` 的 SMC 失效）；AOT 臂**没有对应物**。

## 为什么单独立单

原先是 `REQ-01M7FRY04PTP9788QZPVPKA5ND`（M-lift-4）的一条子项，但那张单是**效率欠账**（`lift_tick` 归零 + 寄存器跨块 SSA），
本条是**静默语义缺口**。"跑错了"与"跑得慢"不是同一类失效 ⇒ 按本仓规矩（一张单只收一类）拆出来。

## 判据（能红）

1. 一条纯夹具用例：合成一段 guest 代码 → 让它执行一次（IR 已发射/已进块）→ 从 guest 侧**改写那段字节** → 再执行
   ⇒ 必须执行**新字节**，或**响亮停**（`decode-error` / 表外 EA / 点名 EA 的 `Stopped`）；⛔ 不许继续执行旧 IR 且不吭声。
2. 若选"响亮停"那条路：必须点名 EA + 原因，并进摘要（⛔ 不许只计数不吭声 —— 与 §12.8 第 3 条同口径）。
3. **现状必红**：今天既不失效也不报错（上面两处注释就是证据；实现者要在报告里给出"改前它为什么不红/怎么红"的读数）。

## 边界

- ⛔ 不做整页写保护 / 软件断点 / 重 lift（构建期不可能）。最小修法是"写点命中已发射块 ⇒ 标记该块失效 ⇒
  下次派发按**响亮停**处理（点名 EA），因为构建期无法重新生成 IR"。
- 与 `M-lift-2` / `M-lift-4` 无关；那两张仍是纯效率，⛔ 不把本条的判据塞进它们。
- 与 `decode_cache.h` 的"自带字节校验"不冲突：那是**只读解码 cache**的自我保护，不是 AOT 执行源的失效。
