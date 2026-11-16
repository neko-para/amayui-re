# B · 手写 native override（函数级 / 段级；语义更优件，不是提速主力）

- id: REQ-01M7CZQZK3QQJBP5XCX88B4CTE
- type: req
- status: dropped
- parent: REQ-01M7CZQVSK0G5EX4XTPW2B9N1W
- dropped_reason: 用户 2026-11-16 裁决：整条 B 支路废弃。B = 手写 native override（函数级/段级），是解释侧机制；AOT 臂自 2026-11-16 起执行来源只有 IR 与 shim（AGENTS §12.8 第 1 条，那套机制已整条删除），解释器已退役 ⇒ 没有合法宿主。三个子单一同 drop；将来若需要'语义更优件'，只能走构建期（改闭包 / 改发射的 IR）。
- tags: [lift, native-override]

## 范围

语义已知、且"手写比 lift 更优"的局部替换：函数级（`NativeOverrideTable`）与段级（`NativeBlockOverride`）。

## 准入（三条，缺一不可）

1. 语义能从语料取证（锚 EA，不是凭印象）；
2. 有差分 oracle：解释器臂同一 EA 的真字节跑出同一结果；
3. 失败必须**回退解释器**，且回退前**零 guest 可见副作用**（`native_overrides.h` 的准入门）。

样板 = CRC-32 两个变体（表从 guest 读、`n<=0` 接管、`INT_MIN` 回退）见 `L3-crc-01`。

## 边界

整 `.text` lift 之后，这里的每一项**仍是"局部更优件"，不是提速主力** —— 提速职责在 A。
所以本支的判据是"语义更优/更稳"，不是"更快"；判据里不许出现"所以整体快了 N 倍"。
