# --until 对 lift_dyn 尾调用 / call 返回点不生效（假阴性）

- id: REQ-01M7NGF0R00R9ZJZ3BMXB8B2R5
- type: bug
- status: open
- parent: REQ-01M7JHSEKXW8KAKB6K2RS645M7
- repro: docs/03-emulator/defects.md#BUG-31
- severity: S3
- tags: [emulator-lift, aot, diagnostics]

## 一句话

`--until` 只在 AOT 派发主循环比较 EA；`lift_dyn` 尾调用的**跳转表 case 体**与 **`call` 返回点**永远停不住，
run 只会 time-limit（假阴性）。

## 现象

- blind wipe 的 4 条 mode 探针（`0x4B1F99 / 0x4B21DB / 0x4B286F / 0x4B2AB5`）全部 time-limit；
  阳性对照 `--until 0x4B06D0`（call 目标 = 块入口）当场命中。
- `D3DXMatrixDecompose` 的 E2E 用 `--until 0x4A9F50 / 0x4AF05A` 4 次尝试全部 time-limit；
  `out/aot/lift_table.inc` 里没有这些 EA。
- 根因：比较只在 `aot/aot_run.cpp`；`runtime/lift_abi.cpp` 的 `lift_dyn` 直接尾调用 `fn`，不检查 until；
  生成代码块内也不检查。完整读数见 `docs/03-emulator/defects.md#BUG-31`。

## 要做

1. 给 `lift_dyn` 加**仅诊断档**的 until 检查（全局 atomic 开关 + 比较 EA；默认档一次 relaxed load），
   或在派发表生成时把"call 返回点"列为可停块入口。⛔ 不要为诊断把检查塞进每个生成块。
2. 测试：一条 CLI/CTest 用例，`--until` 在**跳转表 case EA** 上命中（不能只测块入口）；
   变异：短路这个检查 ⇒ 用例红。
3. 文档：`--until` 的正确用法（派发块入口 / `call` 后紧随入口），并写清当前不支持的形式。

## 判据

- 新用例在 `--until <case EA>` 上命中、exit 0；默认档的 `lift_dyn` 只多一次 relaxed load（静态/墙钟读数）。
- 变异实测红。
- `--time-limit` 是否同族（长 shim / fence 等待中不轮询）单独列为待核，⛔ 不混进本单的完成条件。

## 未做

- 若 `--time-limit` 确认同样只在主循环轮询，另开一条（不要在本单悄悄扩大范围）。
