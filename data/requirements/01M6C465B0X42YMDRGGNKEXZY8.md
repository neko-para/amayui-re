# 🐞 GAMESTART 说明文字画到屏幕左上角（0x198 被当成文本槽偏移）

- id: REQ-01M6C465B0X42YMDRGGNKEXZY8
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: apps/emulator-cxx/CMakeLists.txt#spec_adv_text_layer_position
- tags: [bug render spec]

# 🐞 GAMESTART 的说明文字画到屏幕左上角（0x198 被当成"文本槽偏移"）

## 症状

进入 GAMESTART 后，鼠标 hover 右侧的开关行（难度 / 单位·城砦 / 道具·持有金钱 / 克隆模式 / 多周目事件 / 敌方 LV 上限解除）：

- **期望**：白色说明文字出现在面板下方那块棕色空闲区里；
- **实际**：说明文字画在 **1280×720 屏幕的左上角**（`(10,14)` 起），棕色区是空的。

## 根因

GAMESTART 的说明文字层配置是：

- `0x70 9 492 92 0 0`：层 9 = 492×92、位置 `(0,0)`；
- `0x198 9 706 425`：把层 9 搬到说明区 `(706,425)`；
- `0x79 9 10 14`：文本原点 `(10,14)`。

`0x198` 在引擎里写的是**层记录 +0x0C / +0x10** —— 与 `0x70` 第 4 / 5 个操作数**同两格**（`sub_456400`：`if (!a2) a2 = Font[0x4CC]`，再 `[rec+0x0C] = a3; [rec+0x10] = a4`）。逐字绘制条目据此算 `x = 层+0x0C + cell.x`。

旧实现把 `0x198` 读成"文本槽的绘制原点偏移"，写进 `TextSlot::x_offset/y_offset`（一个**没有消费者**的字段），层记录的位置仍是 `(0,0)` ⇒ 条目落在 `(10,14)`。

## 修复

- `apps/emulator-cxx/src/core/vm_ops_text.cpp` 的 `op_198`：写 `list_records_[group].f0c/f10`；层号 0 ⇒ 默认层（`default_group_`，与 `0x70/0x71/0x79` 同一个选择子），不是"槽 1"。
- 删掉 `TextSlot::x_offset/y_offset` 与两个观察口（无消费者的死状态）。
- spec：`60-ops-forward` 的该条改为「移动文本层」；`62-adv-text` 的层矩形条补上 `0x198` 这个写者；bindings / audit 的 `what` / falsifier / 残差同步。
- 守卫：新 CTest `spec_adv_text_layer_position`（`--only-text-layer-pos`）断言层记录两格、宽高不变、默认层选择子、以及第 1 格逐字条目的位置 `(716,439)`；聚合成员 `emulator-core-selftest` 里也更新了 0x198 的两段。

## 验证

- `ctest --test-dir apps/emulator-cxx/out`：102/102 通过（`spec_adv_text_layer_position` 在列）。
- 变异：把 `record.f0c = left` 改成 `left + 1` ⇒ 该 CTest 当场红（`层 9 的左/上 = (707,425)`）。
- 端到端：route → GAMESTART 后 hover（普通 / 克隆模式 ON 两处）导出 BMP，说明文字落在棕色区 `(716,439)` 起。
- `pnpm tools spec validate` 13/13；`pnpm typecheck` 绿；`pnpm test` 1141/1141。

## 怎么查

`pnpm tools spec show CORE-OPS-FORWARD-TEXT-SLOT` · `data/spec/chapters/62-adv-text.md` 的「文本层：矩形、原点、清空与画布」· CTest `spec_adv_text_layer_position`。
