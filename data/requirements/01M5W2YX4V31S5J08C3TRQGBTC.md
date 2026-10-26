# 🐞正常退出不写 SYS4REG.INI：spec 缺退出触发 + emulator-cxx 无保存调用

- id: REQ-01M5W2YX4V31S5J08C3TRQGBTC
- type: bug
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: tools/test/config-exit-save-contract.test.mjs#★ 窗口关闭：已装载配置 ⇒ 先保存、再清理
- repro: tools/test/config-exit-save-contract.test.mjs#★ 窗口关闭：已装载配置 ⇒ 先保存、再清理
- severity: S2
- done_reason: spec 新增 CORE-CONFIG-EXIT-SAVE；C++ ConfigStore::save_player_file 按固定段序/条件键整体重写；boot 在 Quit 或根帧 Halted 时保存；C++ selftest 保存合同 + pure config-exit-save-contract。
- tags: [config, exit, emulator-cxx]

## 现象

引擎在窗口关闭收尾里调用 `Reg::Save` 写玩家 `SYS4REG.INI`；emulator-cxx 之前只读配置、没有保存调用，窗口 Quit / 根帧 `exit` 都不写盘。spec 的 `CORE-CONFIG-PERSIST` 还把“保存由哪个宿主路径触发”列为不在本规则内，触发合同缺失。

## 取证

- `sub_4057B0`（0x405A2B / 0x405B78）在退出路径取配置对象 `vtable[+0x28]` 调用保存，之后才走退出清理。
- 保存实现 `sub_490590` → `sub_495AD0`；文件路径由 `sub_4900F0` 选 `SYS4REG.INI`。

## 落点

- spec 新规则 `CORE-CONFIG-EXIT-SAVE`：窗口关闭消息的引擎收尾且配置库已建立 ⇒ 调一次保存，先于销毁窗口；宿主调试上限不触发。根帧结束经主循环捕获后向窗口发 WM_CLOSE（0x4138B7），同走关闭收尾。
- C++ `ConfigStore::save_player_file` 按 `CORE-CONFIG-PERSIST` 的 53 个持久键 / 固定段序 / 条件键整体重写；`run_windowed_frames` 返回 `user_quit`，`boot.cpp` 在 Quit 或根帧 `Halted` 时保存。实测默认路径写出 **903 B**，与 `.tmp/SYS4REG.INI` 逐字节一致。
- 守卫：`core-selftest` 的保存合同测试（CRLF / 条件键 / 段序）；pure `config-exit-save-contract`（保存先于清理）。

## 残差

- `--play-frames N` 的宿主帧上限不写盘（合同明确）；真实 Quit 的端到端写盘仍需窗口手工确认。
