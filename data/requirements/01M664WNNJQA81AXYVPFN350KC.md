# 🐞 文本层绘制条目的键基数判据过窄（脚本没写过 0x213 就一条都不建）

- id: REQ-01M664WNNJQA81AXYVPFN350KC
- type: bug
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: apps/emulator-cxx/tests/core_selftest.cpp#check_text_layer_fallback
- repro: docs/02-engine/repro/adv-message-text-invisible.md#观测点
- severity: S1
- tags: [emulator-cxx, adv, text]

## 结论（纠正原判断）

**原先把这条当成"ADV 点击丢失"的根因是错的。** 用 `links/install` 实跑：ADV 消息层**确实调了 `0x213`**
（把判据改回 `key_base_set` 后，正文 BMP 与修好后**逐字节相同**）⇒ 它不影响这一场的文字显示。

但判据本身仍然是错的：`key_base_set` 的语义是"脚本写过这条指令"，而绘制 / 擦除要的是
"`f68` 现在可用"。`0x70` 会给**没写过**的层补一个宿主兜底基数（这正是残差
`ADV-TEXT-LAYER-DRAW-KEY` 的存在证明）；拿"脚本写过"当判据，会把这类层的整段文字吞掉，
而且**症状与"点击没反应"无法区分**（画面不变）。

## 修法

`ListRecord` 拆出 `key_base_set`（脚本写过）与 `key_base_fallback`（兜底写过），
`key_base_ready()` 取或；`0x70` 补兜底时同时置后者；绘制 / 擦除只看 `key_base_ready()`。

## 判据

- CTest `emulator-core-text-layer-fallback`：**没有** `0x213` 的层必须在兜底键上出绘制条目；
  有 `0x213` 时条目落在脚本键上（兜底不许覆盖脚本值）。
- 变异实测：把判据改回 `key_base_set` ⇒ 该 CTest 当场红（"一条绘制条目都没建"）。
- 反证（实跑 `links/install`）：把 `create_text_layer_item` 改成**无条件早退** ⇒ ADV 正文整段消失
  （BMP 只剩背景 + 右侧菜单），证明这条链就是文字的呈现路径。

## 另见

真正的"点击丢失"是另一条：`REQ-01M665WGT6R9WH94F4QGTDYX7Y`（等待位 bit10 在长动画期间阻塞派发）。
