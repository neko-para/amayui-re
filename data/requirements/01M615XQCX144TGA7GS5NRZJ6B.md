# 设置界面底部示例文本：ADV 文本层 spec + emulator-cxx 实现

- id: REQ-01M615XQCX144TGA7GS5NRZJ6B
- type: req
- status: done
- parent: REQ-01M5Z5D786WF65YWHFK7K8Z7Q8
- verify: apps/emulator-cxx/CMakeLists.txt#emulator-core-config-sample-text

# 设置界面底部示例文本：ADV 文本层 spec + emulator-cxx 实现

- id: REQ-01M615XQCX144TGA7GS5NRZJ6B
- type: req
- status: done
- parent: REQ-01M5Z5D786WF65YWHFK7K8Z7Q8
- verify: apps/emulator-cxx/CMakeLists.txt#emulator-core-config-sample-text

## 交付

- **spec**：`repo:data/spec/chapters/62-adv-text.md`（`ADV-TEXT-LAYER` / `ADV-TEXT-APPEND`）。
  文本层 = 十槽记录（`Font+0x414+4*slot`，槽 0 = 默认槽）；`0x70` 设矩形并建/重建纹理 `20+slot`；
  `0x79` 设文本原点；`0x213` 设绘制键区间；`0x71` 清画布 / 复位游标 / 让画布重新被消费。
- **实现**：`apps/emulator-cxx`（`vm_ops_text.cpp` 的 `op_70/op_71/op_79/op_213` 辅助 + `op_6e/op_196/op_6f`；
  `text_raster.cpp` 增加 `advance_out` / 字号覆盖，供振り仮名用 B 支字号）。
- **守卫**：`core_selftest` 的 `check_text_layer_surface()`（pure；结构 + 清空 + 游标 + 绘制项）；
  `emulator-core-config-sample-text` CTest（assets；设置界面纹理 29 必须 824×120 且**有墨**）。

## 判据

- `ctest` 的 `emulator-core-config-sample-text` 绿；`core-selftest` 绿。
- `pnpm test` / `pnpm tools spec validate` / `pnpm tools requirements validate` 绿。
- 变异实测：把 `draw_text_layer_text` 改成 no-op ⇒ `core-selftest` 与 sample-text CTest 同时红。

## 已知残差（进 spec audit）

- 引擎的逐字符绘制 / typewriter 推进（`sub_45BE20` / `sub_4ACE50`）未建模；本实现一次画完整段。
- 自动折行 / 对齐 / 字距 / 振り仮名按行比例切分（`sub_465A20`）未建模；超出画布按边界裁掉。
- 旧 DirectDraw Blt 路径（`sub_43E260`）与 `Engine+0xA30D0` 的选择条件未取证。
- `0x213` 未写键基数时画布是否可见、与脚本自建 20..29 纹理撞号的覆盖顺序未取证。
