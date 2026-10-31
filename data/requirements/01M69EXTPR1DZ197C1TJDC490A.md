# emulator-cxx 与 spec 收口轮对齐：待做与已知偏差

- id: REQ-01M69EXTPR1DZ197C1TJDC490A
- type: req
- status: dropped
- parent: REQ-01M674XHSX2HSFWSKDWZV2C63D
- dropped_reason: 内容已并入交接单 REQ-01M5BK8QMQYK7XQG707YVMQ202 的「2026-11-01 TITLE 收口轮：emulator 侧待做 / 已知偏差」；按用户口径不单独留单。
- tags: [emulator-cxx]

# emulator-cxx 与 spec 收口轮对齐（2026-11）

本单收集**这一轮没做完 / 明确不做 / 已知偏差**的项（SCOPE.md §3.1 逐条对应）。
已完成并挂守卫的：输入合成（`spec_input_composition`）、文本容器与字体名（`spec_text_container_font_names`）、
纹理装载（`spec_texture_set_load`）、帧流程（`spec_frame_exit_load_flow`）、
文本层模式门与 0x70 读序（`spec_adv_text_layer_mode`）、错误通道三步判定（`spec_error_channel_write`）。

## 待做（按优先级）

1. `0x25b` 的**外枠纹理**与显示状态 `1` 那一支：外枠编号格、每次请求重建、失败经错误通道（spec `60-ops-render` 的「外枠纹理」）。
2. `0x79` / `0x1c1` / `0x212` / `0x25d` / `0x303` 的字段角色（槽位下标与键区间）逐条复核。
3. ADV 文本层的**数量门**（揭示游标 < 键数量才建条目）—— 需要先定"本作 ADV 消息层是否调过 `0x213`"（残差 `RES-ADV-TEXT-LEGACY-PATH`）。
4. 字体度量后端接真（FreeType/GDI）→ `0x75`/`0x197`/`0x1a5`/`0x2fe` 的派生例程不再恒走回退。
5. 数组地址算术的 32 位回绕独立用例（`0x61` / `0x12c`）。

## 已知偏差（实现侧选择，待上游正文或取证）

- 表 B（逻辑键号 → 系统键码）只登记 7 项，未知逻辑键号只诊断不写表 C。
- 鼠标左右键交换没有配置键（引擎一格 + 注入口）。
- 摇杆轴方向约定（X 正 = 右 / Y 负 = 上）与中位阈值是本实现的。
- 「再缩一次」分支取"对位模式 = 1 再缩"。
- `display:TextureBit` 缺键按 32 位。
- `0x2` 的 −11 支位置表 / 返回点表走宿主接缝。
- ADV 旧路径下 `0x6e` / `0x196` 仍会经宿主兜底建画布。
