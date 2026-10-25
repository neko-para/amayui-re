# emulator-cxx · 池翻页 0x6D（两组 bank）

- id: REQ-01M5T4RK11TN1PQFG24EEC8Y44
- type: req
- status: open
- parent: REQ-01M5SZ3M7GCR0M23RSYBD7GEA5
- verify: tools/test/emulator-cxx-dispatch.test.mjs#★ 分派表 ↔ handler 声明：两个方向都不许漏

## 判据

- `core_selftest` 的 `check_memflip`：默认配置（键缺失 = 0）⇒ `0x6d` 抛 `display-message:memflip…`、选择不变；启动期键 = 1 ⇒ 翻页只换选择（不搬数据），再翻一次回原组、原值仍在。
- `tools/test/emulator-cxx-dispatch.test.mjs`。

## 实现

- 六个全局族两组 bank：地址空间 `0x10000000 + bank*0x18000000 + (tag-3)*0x04000000`（一组 = 6 × 0x04000000，两组正好铺到局部池起点 0x40000000）。
- 存储共用一张表、**键带 bank**（`bank_key(index)`）；`resolve_address` 解出 bank 并把 bank 编进键 ⇒ 所有按地址的裸访问（`read_raw` / `store_raw` / `*_at`）自动落在正确的 bank。
- `0x6d` 在**执行期**再读一次配置键；`0x09` 只清当前选中 bank（另一组保留）。
- 备组未登记（启动期键为 0）时，落到备组窗口的裸地址访问**响亮失败**（⛔ 不补分配）。

## 待核

- 跨翻页的引用槽归属（存进引用槽的地址在翻页后指哪一组）：规范**没有**规定；本实现让地址自带 bank（地址里本来就有），参考实现同形 ⇒ 记为实现选择。
- `0x09` 与两组 bank 的交互：规范只说"整体复位"，本实现按参考实现只清当前组。
